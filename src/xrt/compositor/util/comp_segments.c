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
	const int64_t rx = (int64_t)seg->x - canvas->x;
	const int64_t ry = (int64_t)seg->y - canvas->y;
	if (rx < 0 || ry < 0) {
		return false;
	}
	const int32_t ax = seg_scale_round(rx, tile_w, canvas->w);
	const int32_t ay = seg_scale_round(ry, tile_h, canvas->h);
	int32_t bx = seg_scale_round(rx + seg->w, tile_w, canvas->w);
	int32_t by = seg_scale_round(ry + seg->h, tile_h, canvas->h);
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
