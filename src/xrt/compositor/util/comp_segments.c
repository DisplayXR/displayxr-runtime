// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window segments (multi-screen M2) — see comp_segments.h.
 * @ingroup comp_util
 */

#include "util/comp_segments.h"

#include "util/u_multi_display.h"

#include "xrt/xrt_plugin.h"

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

	/*
	 * Mirrored outputs (two screens with the SAME desktop rect) would yield
	 * two overlapping segments for one region. Keep one per rect: the primary
	 * if it is among them, else the first listed. map[] takes a slice's
	 * monitor index back to the caller's screen index.
	 */
	struct u_md_rect monitors[COMP_SEGMENTS_MAX_SCREENS];
	uint32_t map[COMP_SEGMENTS_MAX_SCREENS];
	uint32_t mon_count = 0;
	for (uint32_t i = 0; i < screen_count; i++) {
		const struct comp_seg_rect *d = &screens[i].desktop;
		const struct u_md_rect r = {d->x, d->y, d->x + (int32_t)d->w, d->y + (int32_t)d->h};
		uint32_t dup = UINT32_MAX;
		for (uint32_t j = 0; j < mon_count; j++) {
			if (monitors[j].left == r.left && monitors[j].top == r.top && monitors[j].right == r.right &&
			    monitors[j].bottom == r.bottom) {
				dup = j;
				break;
			}
		}
		if (dup == UINT32_MAX) {
			monitors[mon_count] = r;
			map[mon_count] = i;
			mon_count++;
		} else if (screens[i].is_primary && !screens[map[dup]].is_primary) {
			map[dup] = i; // the primary wins a mirror
		}
	}

	struct u_md_slice slices[U_MD_MAX_SLICES];
	const uint32_t n = u_multi_display_compute_slices(canvas_desktop, monitors, mon_count, slices,
	                                                  U_MD_MAX_SLICES < COMP_SEGMENTS_MAX ? U_MD_MAX_SLICES
	                                                                                     : COMP_SEGMENTS_MAX);

	for (uint32_t i = 0; i < n; i++) {
		const struct u_md_slice *s = &slices[i];
		const uint32_t si = map[s->monitor_index];
		const struct comp_segments_screen *scr = &screens[si];
		struct comp_segment *g = &out->seg[out->count++];
		g->screen_index = si;
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

bool
comp_segments_screen_covered(const struct comp_segment_table *t, uint64_t screen_id)
{
	if (t == NULL || t->count == 0) {
		return true; // no coverage information: the session's own wish
	}
	for (uint32_t k = 0; k < t->count && k < COMP_SEGMENTS_MAX; k++) {
		const struct comp_segment *g = &t->seg[k];
		if (g->screen_id == screen_id && g->window_rect.w > 0 && g->window_rect.h > 0) {
			return true;
		}
	}
	return false;
}

void
comp_segments_cover_dwell_init(struct comp_segments_cover_dwell *d, uint64_t hold_ns)
{
	if (d == NULL) {
		return;
	}
	memset(d, 0, sizeof(*d));
	d->hold_ns = hold_ns > 0 ? hold_ns : COMP_SEGMENTS_DEFAULT_HANDOFF_HOLD_NS;
}

uint32_t
comp_segments_cover_dwell_update(struct comp_segments_cover_dwell *d,
                                 const struct comp_segment_table *t,
                                 const struct comp_segments_screen *screens,
                                 uint32_t screen_count,
                                 uint64_t now_ns)
{
	if (d == NULL || screens == NULL) {
		return 0;
	}
	if (d->hold_ns == 0) {
		d->hold_ns = COMP_SEGMENTS_DEFAULT_HANDOFF_HOLD_NS; // zero-initialised: the default
	}
	if (screen_count > COMP_SEGMENTS_MAX_SCREENS) {
		screen_count = COMP_SEGMENTS_MAX_SCREENS;
	}
	uint32_t changed = 0;
	for (uint32_t i = 0; i < screen_count; i++) {
		const bool was_uncovered = d->uncovered[i];
		if (comp_segments_screen_covered(t, screens[i].id)) {
			// Covered: at once, cancelling any pending 2D.
			d->pending[i] = false;
			d->uncovered[i] = false;
		} else if (!d->uncovered[i]) {
			if (!d->pending[i]) {
				d->pending[i] = true;
				d->since_ns[i] = now_ns;
			}
			if (now_ns - d->since_ns[i] >= d->hold_ns) {
				d->pending[i] = false;
				d->uncovered[i] = true;
			}
		}
		if (d->uncovered[i] != was_uncovered) {
			changed |= 1u << i;
		}
	}
	return changed;
}

bool
comp_segments_window_is_split(const struct comp_seg_rect *window_desktop,
                              const struct comp_seg_rect *canvas,
                              const struct comp_segments_screen *screens,
                              uint32_t screen_count)
{
	struct comp_segment_table t;
	if (window_desktop == NULL || screens == NULL || screen_count == 0) {
		return false;
	}
	memset(&t, 0, sizeof(t));
	comp_segments_compute(window_desktop, canvas, screens, screen_count, &t);
	const uint32_t cw = canvas != NULL && canvas->w > 0 ? canvas->w : window_desktop->w;
	const uint32_t ch = canvas != NULL && canvas->h > 0 ? canvas->h : window_desktop->h;
	return comp_segments_table_is_split(&t, cw, ch);
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

static void
sort_i32(int32_t *v, uint32_t n)
{
	for (uint32_t i = 1; i < n; i++) {
		int32_t k = v[i];
		uint32_t j = i;
		while (j > 0 && v[j - 1] > k) {
			v[j] = v[j - 1];
			j--;
		}
		v[j] = k;
	}
}

uint32_t
comp_segments_uncovered(const struct comp_segment_table *t,
                        const struct comp_seg_rect *canvas,
                        struct comp_seg_rect *out,
                        uint32_t cap)
{
	if (canvas == NULL || out == NULL || cap == 0 || canvas->w == 0 || canvas->h == 0) {
		return 0;
	}
	const int32_t cx0 = canvas->x, cy0 = canvas->y;
	const int32_t cx1 = canvas->x + (int32_t)canvas->w, cy1 = canvas->y + (int32_t)canvas->h;
	const uint32_t n = t != NULL ? t->count : 0;

	// Row bands at every segment's top/bottom edge.
	int32_t ys[2 * COMP_SEGMENTS_MAX + 2];
	uint32_t yn = 0;
	ys[yn++] = cy0;
	ys[yn++] = cy1;
	for (uint32_t i = 0; i < n; i++) {
		const struct comp_seg_rect *r = &t->seg[i].window_rect;
		if (r->y > cy0 && r->y < cy1) {
			ys[yn++] = r->y;
		}
		const int32_t b = r->y + (int32_t)r->h;
		if (b > cy0 && b < cy1) {
			ys[yn++] = b;
		}
	}
	sort_i32(ys, yn);

	uint32_t count = 0;
	for (uint32_t b = 0; b + 1 < yn; b++) {
		const int32_t y0 = ys[b], y1 = ys[b + 1];
		if (y1 <= y0) {
			continue;
		}
		// Segments spanning this band, as sorted x intervals.
		int32_t xs0[COMP_SEGMENTS_MAX], xs1[COMP_SEGMENTS_MAX];
		uint32_t xn = 0;
		for (uint32_t i = 0; i < n; i++) {
			const struct comp_seg_rect *r = &t->seg[i].window_rect;
			if (r->w == 0 || r->h == 0 || r->y > y0 || r->y + (int32_t)r->h < y1) {
				continue;
			}
			uint32_t j = xn++;
			while (j > 0 && xs0[j - 1] > r->x) {
				xs0[j] = xs0[j - 1];
				xs1[j] = xs1[j - 1];
				j--;
			}
			xs0[j] = r->x;
			xs1[j] = r->x + (int32_t)r->w;
		}
		int32_t x = cx0;
		for (uint32_t i = 0; i <= xn; i++) {
			const int32_t gap_end = i < xn ? (xs0[i] < cx1 ? xs0[i] : cx1) : cx1;
			if (gap_end > x && count < cap) {
				out[count++] = (struct comp_seg_rect){x, y0, (uint32_t)(gap_end - x), (uint32_t)(y1 - y0)};
			}
			if (i < xn && xs1[i] > x) {
				x = xs1[i];
			}
		}
	}
	return count;
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

int
comp_segments_session_dp_screen(const struct comp_segments_session_dp *sd,
                                const struct comp_segments_session_candidate *cands,
                                uint32_t n,
                                enum comp_segments_session_miss *out_miss)
{
	enum comp_segments_session_miss miss = COMP_SEG_SESSION_GONE;
	int found = -1;
	if (sd == NULL || !sd->valid) {
		miss = COMP_SEG_SESSION_NOT_RECORDED;
	} else {
		for (uint32_t i = 0; cands != NULL && i < n && found < 0; i++) {
			const struct comp_segments_session_candidate *c = &cands[i];
			if (c->monitor_id != sd->monitor_id) {
				continue;
			}
			if (c->not_ready) {
				// Nothing may weave a panel its plug-in cannot drive yet —
				// not even the session DP that was made for it.
				miss = COMP_SEG_SESSION_NOT_READY;
				continue;
			}
			if (c->plugin_id == NULL || strcmp(c->plugin_id, sd->plugin_id) != 0) {
				miss = COMP_SEG_SESSION_RECLAIMED;
				continue;
			}
			const char *serial = c->serial != NULL ? c->serial : "";
			if (strcmp(serial, sd->serial) != 0 || c->platform_id != sd->platform_id ||
			    c->desktop.x != sd->desktop.x || c->desktop.y != sd->desktop.y ||
			    c->desktop.w != sd->desktop.w || c->desktop.h != sd->desktop.h) {
				miss = COMP_SEG_SESSION_REBOUND;
				continue;
			}
			found = (int)i;
			miss = COMP_SEG_SESSION_OWNS;
		}
	}
	if (out_miss != NULL) {
		*out_miss = miss;
	}
	return found;
}

const char *
comp_segments_session_miss_str(enum comp_segments_session_miss miss)
{
	switch (miss) {
	case COMP_SEG_SESSION_OWNS: return "owns";
	case COMP_SEG_SESSION_NOT_RECORDED: return "not recorded";
	case COMP_SEG_SESSION_GONE: return "gone";
	case COMP_SEG_SESSION_RECLAIMED: return "re-claimed";
	case COMP_SEG_SESSION_REBOUND: return "re-bound";
	case COMP_SEG_SESSION_NOT_READY: return "not ready (flat 2D until its plug-in can drive it)";
	}
	return "?";
}

bool
comp_segments_lifecycle_adopt(struct comp_segments_lifecycle *lc, uint64_t screen_id)
{
	if (lc == NULL) {
		return false;
	}
	struct comp_segments_slot *s = lc_find(lc, screen_id, true);
	if (s == NULL) {
		return false;
	}
	s->live = true;
	s->failed = false;
	s->present_streak = lc->create_after;
	s->absent_streak = 0;
	return true;
}

bool
comp_segments_dp_key_keeps(const struct comp_segments_dp_key *was, const struct comp_segments_dp_key *now)
{
	if (was == NULL || now == NULL || was->binding == NULL || now->binding == NULL) {
		return false;
	}
	if (was->screen_id != now->screen_id || was->iface != now->iface || was->inst != now->inst ||
	    was->confidence != now->confidence) {
		return false;
	}
	// The whole binding but its EDID mm (refined late on macOS, see the header).
	struct xrt_screen_binding a;
	struct xrt_screen_binding b;
	memcpy(&a, was->binding, sizeof(a)); // bytes, padding included: memcmp below
	memcpy(&b, now->binding, sizeof(b));
	a.physical_width_mm = b.physical_width_mm = 0;
	a.physical_height_mm = b.physical_height_mm = 0;
	return memcmp(&a, &b, sizeof(a)) == 0;
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


/*
 *
 * Window-handle ownership.
 *
 */

static uint64_t
seg_area(const struct comp_segment *g)
{
	return (uint64_t)g->window_rect.w * (uint64_t)g->window_rect.h;
}

uint32_t
comp_segments_majority(const struct comp_segment_table *t)
{
	if (t == NULL || t->count == 0) {
		return UINT32_MAX;
	}
	uint32_t best = 0;
	uint64_t best_area = 0;
	bool best_primary = false;
	for (uint32_t i = 0; i < t->count && i < COMP_SEGMENTS_MAX; i++) {
		const uint64_t area = seg_area(&t->seg[i]);
		const bool primary = t->seg[i].is_primary;
		if (i == 0 || area > best_area || (area == best_area && primary && !best_primary)) {
			best = i;
			best_area = area;
			best_primary = primary;
		}
	}
	return best;
}

float
comp_segments_window_refresh_hz(const struct comp_seg_rect *window_desktop,
                                const struct comp_segments_screen *screens,
                                const float *screen_hz,
                                uint32_t screen_count,
                                float primary_hz,
                                uint32_t *out_screen_index)
{
	if (out_screen_index != NULL) {
		*out_screen_index = UINT32_MAX;
	}
	if (screen_hz == NULL) {
		return primary_hz;
	}
	struct comp_segment_table t;
	if (comp_segments_compute(window_desktop, NULL, screens, screen_count, &t) == 0) {
		return primary_hz;
	}
	const uint32_t k = comp_segments_majority(&t);
	if (k == UINT32_MAX) {
		return primary_hz;
	}
	const uint32_t i = t.seg[k].screen_index;
	if (out_screen_index != NULL) {
		*out_screen_index = i;
	}
	if (t.seg[k].is_primary || screen_hz[i] <= 0.0f) {
		return primary_hz;
	}
	return screen_hz[i];
}

float
comp_segments_gated_refresh_hz(float screen_hz,
                               uint64_t screen_adapter_luid,
                               uint64_t chain_adapter_luid,
                               float primary_hz,
                               bool *out_other_adapter)
{
	const bool known = screen_adapter_luid != 0 && chain_adapter_luid != 0;
	const bool other = known && screen_adapter_luid != chain_adapter_luid;
	if (out_other_adapter != NULL) {
		*out_other_adapter = other;
	}
	if (!known || other || screen_hz <= 0.0f) {
		return primary_hz;
	}
	return screen_hz;
}

float
comp_segments_screen_refresh_hz(const struct comp_seg_rect *screen_desktop,
                                const struct comp_segments_screen *screens,
                                const float *screen_hz,
                                uint32_t screen_count)
{
	if (screen_desktop == NULL || screens == NULL || screen_hz == NULL || screen_desktop->w == 0 ||
	    screen_desktop->h == 0) {
		return 0.0f;
	}
	const int64_t cx = (int64_t)screen_desktop->x + (int64_t)(screen_desktop->w / 2);
	const int64_t cy = (int64_t)screen_desktop->y + (int64_t)(screen_desktop->h / 2);
	for (uint32_t i = 0; i < screen_count; i++) {
		const struct comp_seg_rect *d = &screens[i].desktop;
		if (cx >= d->x && cx < (int64_t)d->x + (int64_t)d->w && cy >= d->y &&
		    cy < (int64_t)d->y + (int64_t)d->h) {
			return screen_hz[i] > 0.0f ? screen_hz[i] : 0.0f;
		}
	}
	return 0.0f;
}

void
comp_segments_owner_init(struct comp_segments_owner *o, uint64_t primary_id, uint64_t hold_ns, uint32_t margin_pct)
{
	if (o == NULL) {
		return;
	}
	memset(o, 0, sizeof(*o));
	o->hold_ns = hold_ns > 0 ? hold_ns : COMP_SEGMENTS_DEFAULT_HANDOFF_HOLD_NS;
	o->margin_pct = margin_pct > 0 ? margin_pct : COMP_SEGMENTS_DEFAULT_HANDOFF_MARGIN_PCT;
	o->owner_id = primary_id;
}

static void
owner_reset_candidate(struct comp_segments_owner *o)
{
	o->candidate_id = 0;
	o->candidate_since_ns = 0;
	o->failed_id = 0;
}

uint64_t
comp_segments_owner_update(struct comp_segments_owner *o, const struct comp_segment_table *t, uint64_t now_ns)
{
	if (o == NULL) {
		return 0;
	}
	const uint32_t mj = comp_segments_majority(t);
	if (mj == UINT32_MAX) {
		// On no screen: nothing to follow, and the clock restarts.
		owner_reset_candidate(o);
		return 0;
	}
	const struct comp_segment *c = &t->seg[mj];
	if (c->screen_id == o->owner_id || !(c->is_primary || c->has_dp_factory)) {
		// The owner holds the majority, or the majority cannot be woven.
		owner_reset_candidate(o);
		return 0;
	}

	uint64_t total = 0;
	uint64_t owner_area = 0;
	for (uint32_t i = 0; i < t->count && i < COMP_SEGMENTS_MAX; i++) {
		const uint64_t a = seg_area(&t->seg[i]);
		total += a;
		if (t->seg[i].screen_id == o->owner_id) {
			owner_area += a;
		}
	}
	const uint64_t cand_area = seg_area(c);
	// cand - owner >= margin% of the on-screen area, in integers.
	if (cand_area <= owner_area || (cand_area - owner_area) * 100u < total * (uint64_t)o->margin_pct) {
		// Inside the dead band: not a clear majority, the clock restarts.
		owner_reset_candidate(o);
		return 0;
	}

	if (c->screen_id != o->candidate_id) {
		o->candidate_id = c->screen_id;
		o->candidate_since_ns = now_ns;
		o->failed_id = 0;
	}
	if (o->failed_id == c->screen_id) {
		return 0;
	}
	if (now_ns - o->candidate_since_ns < o->hold_ns) {
		return 0;
	}
	return c->screen_id;
}

void
comp_segments_owner_set_result(struct comp_segments_owner *o, uint64_t target_id, bool ok, uint64_t owner_id)
{
	if (o == NULL) {
		return;
	}
	o->owner_id = owner_id;
	if (ok) {
		owner_reset_candidate(o);
	} else {
		// Keep timing the same challenger, but do not retry it until the
		// majority leaves it.
		o->failed_id = target_id;
	}
}

bool
comp_segments_owner_needs_windowless_replacement(const struct comp_segment_table *t,
                                                 uint64_t owner_id,
                                                 bool owner_is_primary,
                                                 bool owner_has_factory)
{
	if (owner_id == 0) {
		return false; // nobody holds the window: nothing to replace
	}
	if (owner_is_primary) {
		return true; // the session always keeps its own DP
	}
	if (!owner_has_factory || t == NULL) {
		return false;
	}
	for (uint32_t k = 0; k < t->count; k++) {
		if (t->seg[k].screen_id == owner_id && t->seg[k].window_rect.w > 0 && t->seg[k].window_rect.h > 0) {
			return true;
		}
	}
	return false;
}

void
comp_segments_next_list_add(struct comp_segments_next_list *nl, uint64_t id, const char *plugin_id, bool is_default)
{
	if (nl == NULL || nl->count >= COMP_SEGMENTS_MAX_SCREENS) {
		return;
	}
	nl->ids[nl->count] = id;
	(void)snprintf(nl->plugin_ids[nl->count], COMP_SEGMENTS_ID_MAX, "%s", plugin_id != NULL ? plugin_id : "");
	nl->count++;
	if (is_default) {
		nl->default_id = id;
	}
}

//! The home is unplugged in @p next: its id is not listed and its plug-in wins no screen (u_screen_home's rule).
static bool
next_home_unplugged(const struct comp_segments_next_list *next, const struct comp_segments_screen *home)
{
	const uint32_t n = next->count < COMP_SEGMENTS_MAX_SCREENS ? next->count : COMP_SEGMENTS_MAX_SCREENS;
	for (uint32_t k = 0; k < n; k++) {
		if (next->ids[k] == home->id || strncmp(next->plugin_ids[k], home->plugin_id, COMP_SEGMENTS_ID_MAX) == 0) {
			return false;
		}
	}
	return true;
}

static bool
next_lists(const struct comp_segments_next_list *next, uint64_t id)
{
	const uint32_t n = next->count < COMP_SEGMENTS_MAX_SCREENS ? next->count : COMP_SEGMENTS_MAX_SCREENS;
	for (uint32_t k = 0; k < n; k++) {
		if (next->ids[k] == id) {
			return true;
		}
	}
	return false;
}

static enum comp_segments_handback
handback_decide(const struct comp_segments_screen *screens,
                uint32_t screen_count,
                uint32_t primary_index,
                int owner_index,
                const bool *ready,
                const struct comp_segment_table *last,
                const struct comp_segments_next_list *next,
                int *target)
{
	*target = -1;
	if (screen_count > COMP_SEGMENTS_MAX_SCREENS) {
		screen_count = COMP_SEGMENTS_MAX_SCREENS;
	}
	if (screens == NULL || ready == NULL || next == NULL || primary_index >= screen_count ||
	    next->default_id == screens[primary_index].id || !next_home_unplugged(next, &screens[primary_index])) {
		return COMP_SEGMENTS_HANDBACK_HOME;
	}

	// Home unplugged. The holder first: the window stays where it is.
	const bool owner_ok = owner_index >= 0 && (uint32_t)owner_index < screen_count;
	if (owner_ok && (uint32_t)owner_index != primary_index && ready[owner_index] &&
	    next_lists(next, screens[owner_index].id)) {
		*target = owner_index;
		return COMP_SEGMENTS_HANDBACK_STAY;
	}

	// Else the ready screen the window covers most.
	uint64_t best_area = 0;
	for (uint32_t i = 0; i < screen_count; i++) {
		if (i == primary_index || !ready[i] || !next_lists(next, screens[i].id)) {
			continue;
		}
		uint64_t area = 0;
		if (last != NULL) {
			for (uint32_t k = 0; k < last->count && k < COMP_SEGMENTS_MAX; k++) {
				if (last->seg[k].screen_id == screens[i].id) {
					area += seg_area(&last->seg[k]);
				}
			}
		}
		if (*target < 0 || area > best_area) {
			*target = (int)i;
			best_area = area;
		}
	}
	if (*target >= 0) {
		return COMP_SEGMENTS_HANDBACK_MOVE;
	}

	// None: the window stays with its holder, whatever that is.
	*target = owner_ok ? owner_index : -1;
	return COMP_SEGMENTS_HANDBACK_NO_READY;
}

enum comp_segments_handback
comp_segments_handback_decide(const struct comp_segments_screen *screens,
                              uint32_t screen_count,
                              uint32_t primary_index,
                              int owner_index,
                              const bool *ready,
                              const struct comp_segment_table *last,
                              const struct comp_segments_next_list *next,
                              int *out_target)
{
	int target = -1;
	const enum comp_segments_handback h =
	    handback_decide(screens, screen_count, primary_index, owner_index, ready, last, next, &target);
	if (out_target != NULL) {
		*out_target = target;
	}
	return h;
}

const char *
comp_segments_handback_str(enum comp_segments_handback h)
{
	switch (h) {
	case COMP_SEGMENTS_HANDBACK_HOME: return "home";
	case COMP_SEGMENTS_HANDBACK_STAY: return "stay";
	case COMP_SEGMENTS_HANDBACK_MOVE: return "move";
	case COMP_SEGMENTS_HANDBACK_NO_READY: return "no ready screen";
	default: return "?";
	}
}

enum comp_segments_away_pick
comp_segments_away_decide(const struct comp_segments_away *away,
                          const struct comp_segments_next_list *next,
                          const bool *can_make,
                          const bool *os_primary,
                          int *out_index)
{
	if (out_index != NULL) {
		*out_index = -1;
	}
	if (away == NULL || away->screen_id == 0 || next == NULL || can_make == NULL || next->default_id != 0) {
		return COMP_SEGMENTS_AWAY_NONE;
	}
	const uint32_t n = next->count < COMP_SEGMENTS_MAX_SCREENS ? next->count : COMP_SEGMENTS_MAX_SCREENS;

	// The panel back: the one screen its plug-in wins.
	int won = -1;
	uint32_t wins = 0;
	if (away->home_plugin_id[0] != '\0') {
		for (uint32_t k = 0; k < n; k++) {
			if (can_make[k] && strncmp(next->plugin_ids[k], away->home_plugin_id, COMP_SEGMENTS_ID_MAX) == 0) {
				won = (int)k;
				wins++;
			}
		}
	}
	if (wins == 1) {
		if (out_index != NULL) {
			*out_index = won;
		}
		return COMP_SEGMENTS_AWAY_PANEL_BACK;
	}

	// The slot's screen is still there: it keeps the slot.
	if (next_lists(next, away->screen_id)) {
		return COMP_SEGMENTS_AWAY_NONE;
	}

	// It left too: where the OS puts the window, else the first that can take it.
	int pick = -1;
	for (uint32_t k = 0; k < n; k++) {
		if (!can_make[k]) {
			continue;
		}
		if (os_primary != NULL && os_primary[k]) {
			pick = (int)k;
			break;
		}
		if (pick < 0) {
			pick = (int)k;
		}
	}
	if (pick < 0) {
		return COMP_SEGMENTS_AWAY_NONE;
	}
	if (out_index != NULL) {
		*out_index = pick;
	}
	return COMP_SEGMENTS_AWAY_HOLDER_GONE;
}

void
comp_segments_away_set(struct comp_segments_away *away,
                       uint64_t screen_id,
                       const char *plugin_id,
                       uint64_t home_id,
                       const char *home_plugin_id)
{
	if (away == NULL) {
		return;
	}
	if (away->screen_id == 0) {
		away->home_id = home_id;
		(void)snprintf(away->home_plugin_id, COMP_SEGMENTS_ID_MAX, "%s",
		               home_plugin_id != NULL ? home_plugin_id : "");
	}
	away->screen_id = screen_id;
	(void)snprintf(away->plugin_id, COMP_SEGMENTS_ID_MAX, "%s", plugin_id != NULL ? plugin_id : "");
	away->slot_other = screen_id != 0 ? 1 : 0;
}

void
comp_segments_away_clear(struct comp_segments_away *away)
{
	if (away == NULL) {
		return;
	}
	away->slot_other = 0;
	away->screen_id = 0;
	away->plugin_id[0] = '\0';
	away->home_id = 0;
	away->home_plugin_id[0] = '\0';
}

bool
comp_segments_away_slot_other(const struct comp_segments_away *away)
{
	return away != NULL && away->slot_other != 0;
}


/*
 *
 * Deferred release.
 *
 */

bool
comp_segments_retire_push(struct comp_segments_retire *r, uint32_t kind, uint64_t item)
{
	if (r == NULL || r->count >= COMP_SEGMENTS_RETIRE_MAX) {
		return false;
	}
	r->kinds[r->count] = kind;
	r->items[r->count] = item;
	r->count++;
	return true;
}

uint32_t
comp_segments_retire_take(struct comp_segments_retire *r,
                          bool safe,
                          uint32_t *out_kinds,
                          uint64_t *out_items,
                          uint32_t cap)
{
	if (r == NULL || !safe || out_kinds == NULL || out_items == NULL || cap == 0) {
		return 0;
	}
	const uint32_t n = r->count < cap ? r->count : cap;
	for (uint32_t i = 0; i < n; i++) {
		out_kinds[i] = r->kinds[i];
		out_items[i] = r->items[i];
	}
	for (uint32_t i = n; i < r->count; i++) {
		r->kinds[i - n] = r->kinds[i];
		r->items[i - n] = r->items[i];
	}
	r->count -= n;
	return n;
}
static int32_t
seg_scale_round(int64_t v, int64_t num, int64_t den)
{
	// round(v * num / den), v/num/den >= 0
	return den > 0 ? (int32_t)((v * num + den / 2) / den) : 0;
}

bool
comp_segments_tile_rect(const struct comp_seg_rect *seg,
                        const struct comp_seg_rect *canvas,
                        uint32_t tile_w,
                        uint32_t tile_h,
                        struct comp_seg_rect *out)
{
	if (seg == NULL || canvas == NULL || out == NULL || canvas->w == 0 || canvas->h == 0) {
		return false;
	}
	// #1575: the part of the segment inside the canvas, on every edge. A
	// segment that starts left of / above the canvas used to be dropped
	// whole; one past the right / bottom edge was already cut (the clamp
	// below), so this only changes a segment that starts outside the canvas.
	const int64_t cx1 = (int64_t)canvas->x + canvas->w;
	const int64_t cy1 = (int64_t)canvas->y + canvas->h;
	const int64_t sx0 = seg->x > canvas->x ? seg->x : canvas->x;
	const int64_t sy0 = seg->y > canvas->y ? seg->y : canvas->y;
	const int64_t sx1 = ((int64_t)seg->x + seg->w) < cx1 ? ((int64_t)seg->x + seg->w) : cx1;
	const int64_t sy1 = ((int64_t)seg->y + seg->h) < cy1 ? ((int64_t)seg->y + seg->h) : cy1;
	if (sx1 <= sx0 || sy1 <= sy0) {
		return false;
	}
	const int64_t rx = sx0 - canvas->x;
	const int64_t ry = sy0 - canvas->y;
	const int32_t ax = seg_scale_round(rx, tile_w, canvas->w);
	const int32_t ay = seg_scale_round(ry, tile_h, canvas->h);
	int32_t bx = seg_scale_round(sx1 - canvas->x, tile_w, canvas->w);
	int32_t by = seg_scale_round(sy1 - canvas->y, tile_h, canvas->h);
	if (bx > (int32_t)tile_w) {
		bx = (int32_t)tile_w;
	}
	if (by > (int32_t)tile_h) {
		by = (int32_t)tile_h;
	}
	if (ax < 0 || ay < 0 || bx <= ax || by <= ay) {
		return false;
	}
	out->x = ax;
	out->y = ay;
	out->w = (uint32_t)(bx - ax);
	out->h = (uint32_t)(by - ay);
	return true;
}

bool
comp_segments_rect_intersect(const struct comp_seg_rect *a, const struct comp_seg_rect *b, struct comp_seg_rect *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (a == NULL || b == NULL || a->w == 0 || a->h == 0 || b->w == 0 || b->h == 0) {
		return false;
	}
	const int64_t x0 = a->x > b->x ? a->x : b->x;
	const int64_t y0 = a->y > b->y ? a->y : b->y;
	const int64_t ax1 = (int64_t)a->x + a->w;
	const int64_t ay1 = (int64_t)a->y + a->h;
	const int64_t bx1 = (int64_t)b->x + b->w;
	const int64_t by1 = (int64_t)b->y + b->h;
	const int64_t x1 = ax1 < bx1 ? ax1 : bx1;
	const int64_t y1 = ay1 < by1 ? ay1 : by1;
	if (x1 <= x0 || y1 <= y0) {
		return false;
	}
	out->x = (int32_t)x0;
	out->y = (int32_t)y0;
	out->w = (uint32_t)(x1 - x0);
	out->h = (uint32_t)(y1 - y0);
	return true;
}

bool
comp_segments_canvas_from_tile(const struct comp_seg_rect *tile,
                               const struct comp_seg_rect *canvas,
                               uint32_t tile_w,
                               uint32_t tile_h,
                               struct comp_seg_rect *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (tile == NULL || canvas == NULL || tile_w == 0 || tile_h == 0 || tile->w == 0 || tile->h == 0 ||
	    tile->x < 0 || tile->y < 0) {
		return false;
	}
	// Each edge where those tile pixels land when the whole tile is woven
	// over the whole canvas (round(v * canvas / tile)).
	const int64_t x0 = seg_scale_round(tile->x, canvas->w, tile_w);
	const int64_t y0 = seg_scale_round(tile->y, canvas->h, tile_h);
	const int64_t x1 = seg_scale_round((int64_t)tile->x + tile->w, canvas->w, tile_w);
	const int64_t y1 = seg_scale_round((int64_t)tile->y + tile->h, canvas->h, tile_h);
	if (x1 <= x0 || y1 <= y0) {
		return false;
	}
	out->x = (int32_t)(canvas->x + x0);
	out->y = (int32_t)(canvas->y + y0);
	out->w = (uint32_t)(x1 - x0);
	out->h = (uint32_t)(y1 - y0);
	return true;
}

static uint32_t
seg_overhang_edge(int64_t v, uint32_t limit)
{
	if (v <= 0) {
		return 0;
	}
	return v < (int64_t)limit ? (uint32_t)v : limit;
}

//! floor(v * num / den) and ceil(v * num / den), v/num/den >= 0.
static int64_t
seg_scale_floor(int64_t v, int64_t num, int64_t den)
{
	return den > 0 ? (v * num) / den : 0;
}

static int64_t
seg_scale_ceil(int64_t v, int64_t num, int64_t den)
{
	return den > 0 ? (v * num + den - 1) / den : 0;
}

/*!
 * Place a cut of @p share tile px over [@p need0, @p need1) inside a tile of
 * @p tile px: at the need's own start, or as far as the tile allows. A cut at
 * least as long as the need always covers it; a shorter one keeps its start.
 */
static int64_t
seg_share_start(int64_t need0, int64_t share, int64_t tile)
{
	const int64_t last = tile - share; // the furthest start that stays in the tile
	return need0 < last ? need0 : last;
}

enum comp_segments_overhang_verdict
comp_segments_overhang_clip(const struct comp_segments_overhang_in *in, struct comp_segments_overhang *out)
{
	if (out == NULL) {
		return COMP_SEG_OVERHANG_NO_INPUT;
	}
	memset(out, 0, sizeof(*out));
	if (in == NULL || in->cols == 0 || in->rows == 0 || in->view_w == 0 || in->view_h == 0 || in->max_w == 0 ||
	    in->max_h == 0 || in->window.w == 0 || in->window.h == 0 || in->canvas.w == 0 || in->canvas.h == 0 ||
	    in->panel.w == 0 || in->panel.h == 0) {
		return COMP_SEG_OVERHANG_NO_INPUT;
	}
	const uint32_t cap_w = in->max_w / in->cols;
	const uint32_t cap_h = in->max_h / in->rows;
	if (cap_w == 0 || cap_h == 0) {
		return COMP_SEG_OVERHANG_NO_INPUT;
	}

	// The whole box fits: carried whole, exactly as before #1575.
	if ((uint64_t)in->cols * in->view_w <= in->max_w && (uint64_t)in->rows * in->view_h <= in->max_h) {
		return COMP_SEG_OVERHANG_FITS;
	}

	// The canvas on the desktop, and the part of it that is on the panel.
	const int64_t cx0 = (int64_t)in->window.x + in->canvas.x;
	const int64_t cy0 = (int64_t)in->window.y + in->canvas.y;
	const int64_t cx1 = cx0 + in->canvas.w;
	const int64_t cy1 = cy0 + in->canvas.h;
	const int64_t px0 = in->panel.x;
	const int64_t py0 = in->panel.y;
	const int64_t px1 = px0 + in->panel.w;
	const int64_t py1 = py0 + in->panel.h;

	out->over_left = seg_overhang_edge(px0 - cx0, in->canvas.w);
	out->over_right = seg_overhang_edge(cx1 - px1, in->canvas.w);
	out->over_top = seg_overhang_edge(py0 - cy0, in->canvas.h);
	out->over_bottom = seg_overhang_edge(cy1 - py1, in->canvas.h);

	const int64_t kx0 = cx0 > px0 ? cx0 : px0;
	const int64_t ky0 = cy0 > py0 ? cy0 : py0;
	const int64_t kx1 = cx1 < px1 ? cx1 : px1;
	const int64_t ky1 = cy1 < py1 ? cy1 : py1;
	if (kx1 <= kx0 || ky1 <= ky0) {
		return COMP_SEG_OVERHANG_OFF_PANEL;
	}
	if (kx0 == cx0 && ky0 == cy0 && kx1 == cx1 && ky1 == cy1) {
		return COMP_SEG_OVERHANG_ON_PANEL;
	}

	// The part on the panel, window px.
	struct comp_seg_rect keep;
	keep.x = (int32_t)(kx0 - in->window.x);
	keep.y = (int32_t)(ky0 - in->window.y);
	keep.w = (uint32_t)(kx1 - kx0);
	keep.h = (uint32_t)(ky1 - ky0);

	// ... in tile px, rounded outward so its mapping back covers it whole.
	const int64_t rx0 = kx0 - cx0;
	const int64_t ry0 = ky0 - cy0;
	const int64_t rx1 = kx1 - cx0;
	const int64_t ry1 = ky1 - cy0;
	int64_t nx0 = seg_scale_floor(rx0, in->view_w, in->canvas.w);
	int64_t ny0 = seg_scale_floor(ry0, in->view_h, in->canvas.h);
	int64_t nx1 = seg_scale_ceil(rx1, in->view_w, in->canvas.w);
	int64_t ny1 = seg_scale_ceil(ry1, in->view_h, in->canvas.h);
	nx1 = nx1 < (int64_t)in->view_w ? nx1 : (int64_t)in->view_w;
	ny1 = ny1 < (int64_t)in->view_h ? ny1 : (int64_t)in->view_h;
	if (nx1 <= nx0 || ny1 <= ny0) {
		return COMP_SEG_OVERHANG_OFF_PANEL; // thinner than one tile pixel
	}
	struct comp_seg_rect need = {(int32_t)nx0, (int32_t)ny0, (uint32_t)(nx1 - nx0), (uint32_t)(ny1 - ny0)};

	/*
	 * The bridge's per-tile share, at its real offset over the need. Its size
	 * depends only on the tile size, never on where the window is, so a drag
	 * across the panel's edge moves the cut and keeps the box: no egress
	 * rebuild and no DP-input re-create per frame of the drag.
	 */
	const int64_t share_w = cap_w < in->view_w ? cap_w : in->view_w;
	const int64_t share_h = cap_h < in->view_h ? cap_h : in->view_h;
	struct comp_seg_rect tile;
	tile.x = (int32_t)seg_share_start(nx0, share_w, in->view_w);
	tile.y = (int32_t)seg_share_start(ny0, share_h, in->view_h);
	tile.w = (uint32_t)share_w;
	tile.h = (uint32_t)share_h;

	// The DP canvas comes from the cut itself, never from keep, so the tile
	// and the canvas share one scale (2873 px on the panel can be 2874 px of
	// tile content).
	struct comp_seg_rect canvas;
	if (!comp_segments_canvas_from_tile(&tile, &in->canvas, in->view_w, in->view_h, &canvas)) {
		return COMP_SEG_OVERHANG_NO_INPUT;
	}

	out->keep = keep;
	out->need = need;
	out->tile = tile;
	out->canvas = canvas;
	out->box_w = in->cols * tile.w;
	out->box_h = in->rows * tile.h;
	return COMP_SEG_OVERHANG_CLIPPED;
}

//! Start a stretch of the proof at @p in's size: both baselines wait for the next frame.
static void
seg_proof_restart(struct comp_segments_overhang_proof *p, const struct comp_segments_overhang_proof_in *in)
{
	p->window_w = in->window_w;
	p->window_h = in->window_h;
	p->cols = in->cols;
	p->rows = in->rows;
	p->view_w = in->view_w;
	p->view_h = in->view_h;
	p->box_w = in->box_w;
	p->box_h = in->box_h;
	p->frames_at_size = 1;
	p->box_changes = 0;
	p->rebuild_base_set = false;
	p->dp_base_set = false;
	p->rebuild_base = 0;
	p->dp_base = 0;
	p->dp_wait = 0;
}

enum comp_segments_overhang_proof_event
comp_segments_overhang_proof_step(struct comp_segments_overhang_proof *p,
                                  const struct comp_segments_overhang_proof_in *in)
{
	if (p == NULL || in == NULL) {
		return COMP_SEG_PROOF_FRAME;
	}
	if (!p->open) {
		memset(p, 0, sizeof(*p));
		p->open = true;
		p->frames = 1;
		seg_proof_restart(p, in);
		return COMP_SEG_PROOF_STARTED;
	}
	p->frames++;

	// A resize: the window or the tiles it was painted at changed size. The
	// renderer follows the window a frame late, so either one counts.
	if (in->window_w != p->window_w || in->window_h != p->window_h || in->cols != p->cols ||
	    in->rows != p->rows || in->view_w != p->view_w || in->view_h != p->view_h) {
		p->resizes++;
		seg_proof_restart(p, in);
		return COMP_SEG_PROOF_RESIZED;
	}
	p->frames_at_size++;

	// The same size, a different box: the cut followed the window's position.
	if (in->box_w != p->box_w || in->box_h != p->box_h) {
		p->box_changes++;
		p->box_w = in->box_w;
		p->box_h = in->box_h;
	}
	if (!p->rebuild_base_set) {
		p->rebuild_base_set = true;
		p->rebuild_base = in->egress_rebuilds;
	}
	if (!p->dp_base_set) {
		if (in->dp_settled || p->dp_wait >= COMP_SEG_OVERHANG_PROOF_DP_WAIT) {
			p->dp_base_set = true;
			p->dp_base = in->dp_creates;
		} else {
			p->dp_wait++;
		}
	}
	return COMP_SEG_PROOF_FRAME;
}

bool
comp_segments_overhang_proof_end(struct comp_segments_overhang_proof *p,
                                 uint64_t egress_rebuilds,
                                 uint64_t dp_creates,
                                 struct comp_segments_overhang_proof_result *out)
{
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
	if (p == NULL || !p->open) {
		return false;
	}
	if (out != NULL) {
		out->frames = p->frames;
		out->frames_at_size = p->frames_at_size;
		out->resizes = p->resizes;
		out->box_w = p->box_w;
		out->box_h = p->box_h;
		out->have_counts = p->rebuild_base_set && p->dp_base_set;
		if (out->have_counts) {
			out->box_changes = p->box_changes;
			out->egress_rebuilds =
			    egress_rebuilds >= p->rebuild_base ? egress_rebuilds - p->rebuild_base : 0;
			out->dp_creates = dp_creates >= p->dp_base ? dp_creates - p->dp_base : 0;
		}
	}
	memset(p, 0, sizeof(*p));
	return true;
}

bool
comp_segments_source_rect(const struct comp_segment *seg,
                          const struct comp_seg_rect *canvas,
                          uint32_t tile_w,
                          uint32_t tile_h,
                          const struct comp_segments_content *content,
                          struct comp_seg_rect *out)
{
	if (seg == NULL) {
		return false;
	}
	if (content != NULL && content->count > 0) {
		const uint32_t n =
		    content->count < COMP_SEGMENTS_CONTENT_MAX ? content->count : COMP_SEGMENTS_CONTENT_MAX;
		for (uint32_t j = 0; j < n; j++) {
			if (content->screen_id[j] == seg->screen_id) {
				// Where this screen's views WERE painted.
				return comp_segments_tile_rect(&content->rect[j], &content->canvas, tile_w, tile_h,
				                               out);
			}
		}
	}
	return comp_segments_tile_rect(&seg->window_rect, canvas, tile_w, tile_h, out);
}

void
comp_segments_content_ring_put(struct comp_segments_content_ring *r,
                               uint64_t seq,
                               const struct comp_segments_content *content)
{
	if (r == NULL || seq == 0) {
		return;
	}
	const uint32_t i = (uint32_t)(seq % COMP_SEGMENTS_CONTENT_RING);
	r->seq[i] = seq;
	if (content != NULL) {
		r->content[i] = *content;
	} else {
		memset(&r->content[i], 0, sizeof(r->content[i]));
	}
}

bool
comp_segments_mask_crop_rect(const struct comp_seg_rect *segment_window,
                             uint32_t win_w,
                             uint32_t win_h,
                             uint32_t mask_w,
                             uint32_t mask_h,
                             struct comp_seg_rect *out_mask_rect)
{
	if (out_mask_rect == NULL) {
		return false;
	}
	memset(out_mask_rect, 0, sizeof(*out_mask_rect));
	if (segment_window == NULL || win_w == 0 || win_h == 0 || mask_w == 0 || mask_h == 0 ||
	    segment_window->w == 0 || segment_window->h == 0) {
		return false;
	}
	// Window px -> mask px, edges rounded outward so a sliver of a 3D zone on a
	// screen is never dropped (the verdict is any-nonzero, so over-covering by
	// one pixel costs nothing; under-covering loses a zone).
	const int64_t x0 = ((int64_t)segment_window->x * (int64_t)mask_w) / (int64_t)win_w;
	const int64_t y0 = ((int64_t)segment_window->y * (int64_t)mask_h) / (int64_t)win_h;
	int64_t x1 = (((int64_t)segment_window->x + (int64_t)segment_window->w) * (int64_t)mask_w + (int64_t)win_w - 1) /
	             (int64_t)win_w;
	int64_t y1 = (((int64_t)segment_window->y + (int64_t)segment_window->h) * (int64_t)mask_h + (int64_t)win_h - 1) /
	             (int64_t)win_h;
	int64_t cx0 = x0 < 0 ? 0 : (x0 > (int64_t)mask_w ? (int64_t)mask_w : x0);
	int64_t cy0 = y0 < 0 ? 0 : (y0 > (int64_t)mask_h ? (int64_t)mask_h : y0);
	if (x1 > (int64_t)mask_w) {
		x1 = (int64_t)mask_w;
	}
	if (y1 > (int64_t)mask_h) {
		y1 = (int64_t)mask_h;
	}
	if (x1 <= cx0 || y1 <= cy0) {
		return false;
	}
	out_mask_rect->x = (int32_t)cx0;
	out_mask_rect->y = (int32_t)cy0;
	out_mask_rect->w = (uint32_t)(x1 - cx0);
	out_mask_rect->h = (uint32_t)(y1 - cy0);
	return true;
}

bool
comp_segments_content_ring_get(const struct comp_segments_content_ring *r,
                               uint64_t seq,
                               struct comp_segments_content *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (r == NULL || seq == 0) {
		return false;
	}
	const uint32_t i = (uint32_t)(seq % COMP_SEGMENTS_CONTENT_RING);
	if (r->seq[i] != seq) {
		return false;
	}
	*out = r->content[i];
	return true;
}
