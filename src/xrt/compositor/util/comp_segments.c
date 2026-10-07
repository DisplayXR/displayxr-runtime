// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window segments (multi-screen M2) — see comp_segments.h.
 * @ingroup comp_util
 */

#include "util/comp_segments.h"

#include "util/u_multi_display.h"

#include <stdio.h>
#include <string.h>


/*
 *
 * Segment table.
 *
 */

enum comp_seg_1to1
comp_segments_screen_1to1(const struct comp_segments_screen *s)
{
	if (s == NULL || s->native_w == 0 || s->native_h == 0 || s->desktop.w == 0 || s->desktop.h == 0) {
		return COMP_SEG_1TO1_UNKNOWN;
	}
	return (s->native_w == s->desktop.w && s->native_h == s->desktop.h) ? COMP_SEG_1TO1_YES : COMP_SEG_1TO1_NO;
}

static bool
seg_before(const struct comp_segment *a, const struct comp_segment *b, const struct comp_seg_rect *window_desktop)
{
	// Desktop position of each segment = window origin + window-px offset.
	const int64_t ax = (int64_t)window_desktop->x + a->window_rect.x;
	const int64_t bx = (int64_t)window_desktop->x + b->window_rect.x;
	if (ax != bx) {
		return ax < bx;
	}
	return a->window_rect.y < b->window_rect.y;
}

uint32_t
comp_segments_compute(const struct comp_seg_rect *window_desktop,
                      const struct comp_seg_rect *canvas,
                      const struct comp_segments_screen *screens,
                      uint32_t screen_count,
                      struct comp_segment_table *out)
{
	if (out == NULL) {
		return 0;
	}
	memset(out, 0, sizeof(*out));
	if (window_desktop == NULL || screens == NULL || screen_count == 0 || window_desktop->w == 0 ||
	    window_desktop->h == 0) {
		return 0;
	}
	if (screen_count > COMP_SEGMENTS_MAX_SCREENS) {
		screen_count = COMP_SEGMENTS_MAX_SCREENS;
	}

	// The canvas inside the window, window px. Zero = the whole window.
	struct comp_seg_rect cv = {0, 0, window_desktop->w, window_desktop->h};
	if (canvas != NULL && canvas->w > 0 && canvas->h > 0) {
		cv = *canvas;
	}

	// The canvas on the desktop, as the slice math's half-open rect.
	const struct u_md_rect canvas_desktop = {
	    .left = window_desktop->x + cv.x,
	    .top = window_desktop->y + cv.y,
	    .right = window_desktop->x + cv.x + (int32_t)cv.w,
	    .bottom = window_desktop->y + cv.y + (int32_t)cv.h,
	};

	struct u_md_rect monitors[COMP_SEGMENTS_MAX_SCREENS];
	for (uint32_t i = 0; i < screen_count; i++) {
		const struct comp_seg_rect *d = &screens[i].desktop;
		monitors[i].left = d->x;
		monitors[i].top = d->y;
		monitors[i].right = d->x + (int32_t)d->w;
		monitors[i].bottom = d->y + (int32_t)d->h;
	}

	struct u_md_slice slices[U_MD_MAX_SLICES];
	const uint32_t n = u_multi_display_compute_slices(canvas_desktop, monitors, screen_count, slices,
	                                                  U_MD_MAX_SLICES < COMP_SEGMENTS_MAX ? U_MD_MAX_SLICES
	                                                                                     : COMP_SEGMENTS_MAX);

	for (uint32_t i = 0; i < n; i++) {
		const struct u_md_slice *s = &slices[i];
		const struct comp_segments_screen *scr = &screens[s->monitor_index];
		struct comp_segment *g = &out->seg[out->count++];
		g->screen_index = (uint32_t)s->monitor_index;
		g->screen_id = scr->id;
		g->is_primary = scr->is_primary;
		g->has_dp_factory = scr->has_dp_factory;
		// atlas_x/y are relative to the CANVAS; the segment rect is window px.
		g->window_rect.x = cv.x + s->atlas_x;
		g->window_rect.y = cv.y + s->atlas_y;
		g->window_rect.w = s->atlas_w;
		g->window_rect.h = s->atlas_h;
		g->screen_rect.x = s->canvas_offset_x;
		g->screen_rect.y = s->canvas_offset_y;
		g->screen_rect.w = s->atlas_w;
		g->screen_rect.h = s->atlas_h;
		g->present_origin_x = window_desktop->x - scr->desktop.x;
		g->present_origin_y = window_desktop->y - scr->desktop.y;
		g->screen_1to1 = comp_segments_screen_1to1(scr);
	}

	// Left to right (insertion sort; at most COMP_SEGMENTS_MAX entries).
	for (uint32_t i = 1; i < out->count; i++) {
		struct comp_segment key = out->seg[i];
		uint32_t j = i;
		while (j > 0 && seg_before(&key, &out->seg[j - 1], window_desktop)) {
			out->seg[j] = out->seg[j - 1];
			j--;
		}
		out->seg[j] = key;
	}
	return out->count;
}

bool
comp_segments_table_is_split(const struct comp_segment_table *t, uint32_t canvas_w, uint32_t canvas_h)
{
	if (t == NULL || t->count == 0) {
		// On no screen at all: nothing to split; the single-DP path stands.
		return false;
	}
	if (t->count > 1) {
		return true;
	}
	const struct comp_segment *g = &t->seg[0];
	if (!g->is_primary) {
		return true;
	}
	// One primary segment. Clipped by the screen edge (window hanging off the
	// desktop) is still the single-DP case: the off-desktop part is invisible,
	// and this is exactly what shipped. So only a non-primary screen splits.
	(void)canvas_w;
	(void)canvas_h;
	return false;
}

enum comp_seg_render
comp_segments_decide(bool have_dp, bool dp_tolerates_resample, enum comp_seg_1to1 screen_1to1)
{
	if (!have_dp) {
		return COMP_SEG_RENDER_FLAT_2D;
	}
	if (screen_1to1 == COMP_SEG_1TO1_NO && !dp_tolerates_resample) {
		return COMP_SEG_RENDER_FLAT_2D;
	}
	return COMP_SEG_RENDER_WEAVE;
}

bool
comp_segments_table_equal(const struct comp_segment_table *a, const struct comp_segment_table *b)
{
	if (a == NULL || b == NULL) {
		return a == b;
	}
	if (a->count != b->count) {
		return false;
	}
	for (uint32_t i = 0; i < a->count; i++) {
		const struct comp_segment *x = &a->seg[i];
		const struct comp_segment *y = &b->seg[i];
		if (x->screen_id != y->screen_id || x->window_rect.x != y->window_rect.x ||
		    x->window_rect.y != y->window_rect.y || x->window_rect.w != y->window_rect.w ||
		    x->window_rect.h != y->window_rect.h || x->present_origin_x != y->present_origin_x ||
		    x->present_origin_y != y->present_origin_y) {
			return false;
		}
	}
	return true;
}

void
comp_segments_table_format(const struct comp_segment_table *t, char *buf, size_t size)
{
	if (buf == NULL || size == 0) {
		return;
	}
	buf[0] = '\0';
	if (t == NULL) {
		return;
	}
	size_t used = 0;
	int w = snprintf(buf, size, "%u segment(s)", t->count);
	if (w < 0) {
		return;
	}
	used = (size_t)w < size ? (size_t)w : size - 1;
	for (uint32_t i = 0; i < t->count && used + 1 < size; i++) {
		const struct comp_segment *g = &t->seg[i];
		w = snprintf(buf + used, size - used, "; [%u] screen 0x%016llx%s canvas %d,%d %ux%u origin %d,%d 1:1=%s", i,
		             (unsigned long long)g->screen_id, g->is_primary ? " (primary)" : "", g->window_rect.x,
		             g->window_rect.y, g->window_rect.w, g->window_rect.h, g->present_origin_x,
		             g->present_origin_y,
		             g->screen_1to1 == COMP_SEG_1TO1_YES  ? "yes"
		             : g->screen_1to1 == COMP_SEG_1TO1_NO ? "no"
		                                                  : "unknown");
		if (w < 0) {
			return;
		}
		used += (size_t)w;
		if (used >= size) {
			used = size - 1;
		}
	}
}


/*
 *
 * Lifecycle.
 *
 */

void
comp_segments_lifecycle_init(struct comp_segments_lifecycle *lc, uint32_t create_after, uint32_t destroy_after)
{
	if (lc == NULL) {
		return;
	}
	memset(lc, 0, sizeof(*lc));
	lc->create_after = create_after > 0 ? create_after : COMP_SEGMENTS_DEFAULT_CREATE_AFTER;
	lc->destroy_after = destroy_after > 0 ? destroy_after : COMP_SEGMENTS_DEFAULT_DESTROY_AFTER;
}

static struct comp_segments_slot *
lc_find(struct comp_segments_lifecycle *lc, uint64_t id, bool add)
{
	struct comp_segments_slot *free_slot = NULL;
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		struct comp_segments_slot *s = &lc->slots[i];
		if (s->used && s->screen_id == id) {
			return s;
		}
		if (!s->used && free_slot == NULL) {
			free_slot = s;
		}
	}
	if (!add || free_slot == NULL) {
		return NULL;
	}
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used = true;
	free_slot->screen_id = id;
	return free_slot;
}

static void
actions_push(struct comp_segments_actions *out, uint64_t id, enum comp_seg_action a)
{
	if (out->count < COMP_SEGMENTS_MAX_SCREENS) {
		out->items[out->count].screen_id = id;
		out->items[out->count].action = a;
		out->count++;
	}
}

void
comp_segments_lifecycle_update(struct comp_segments_lifecycle *lc,
                               const struct comp_segment_table *t,
                               struct comp_segments_actions *out)
{
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
	if (lc == NULL || out == NULL) {
		return;
	}

	// Mark the screens with a non-empty, non-primary, DP-capable segment.
	bool seen[COMP_SEGMENTS_MAX_SCREENS] = {false};
	if (t != NULL) {
		for (uint32_t i = 0; i < t->count; i++) {
			const struct comp_segment *g = &t->seg[i];
			if (g->is_primary || !g->has_dp_factory || g->window_rect.w == 0 || g->window_rect.h == 0) {
				continue;
			}
			struct comp_segments_slot *s = lc_find(lc, g->screen_id, true);
			if (s == NULL) {
				continue; // out of slots: that segment stays 2D
			}
			seen[s - lc->slots] = true;
		}
	}

	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		struct comp_segments_slot *s = &lc->slots[i];
		if (!s->used) {
			continue;
		}
		if (seen[i]) {
			s->absent_streak = 0;
			if (s->present_streak < UINT32_MAX) {
				s->present_streak++;
			}
			if (!s->live && !s->failed && s->present_streak >= lc->create_after) {
				actions_push(out, s->screen_id, COMP_SEG_ACTION_CREATE);
			}
		} else {
			s->present_streak = 0;
			s->failed = false; // gone: a later return may try again
			if (s->absent_streak < UINT32_MAX) {
				s->absent_streak++;
			}
			if (s->live && s->absent_streak >= lc->destroy_after) {
				actions_push(out, s->screen_id, COMP_SEG_ACTION_DESTROY);
				s->live = false;
			}
			if (!s->live) {
				s->used = false; // nothing left to track
			}
		}
	}
}

void
comp_segments_lifecycle_set_created(struct comp_segments_lifecycle *lc, uint64_t screen_id, bool ok)
{
	if (lc == NULL) {
		return;
	}
	struct comp_segments_slot *s = lc_find(lc, screen_id, false);
	if (s == NULL) {
		return;
	}
	s->live = ok;
	s->failed = !ok;
}

bool
comp_segments_lifecycle_is_live(const struct comp_segments_lifecycle *lc, uint64_t screen_id)
{
	if (lc == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		if (lc->slots[i].used && lc->slots[i].screen_id == screen_id) {
			return lc->slots[i].live;
		}
	}
	return false;
}

void
comp_segments_lifecycle_drain(struct comp_segments_lifecycle *lc, struct comp_segments_actions *out)
{
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
	if (lc == NULL) {
		return;
	}
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		struct comp_segments_slot *s = &lc->slots[i];
		if (s->used && s->live && out != NULL) {
			actions_push(out, s->screen_id, COMP_SEG_ACTION_DESTROY);
		}
		memset(s, 0, sizeof(*s));
	}
}
