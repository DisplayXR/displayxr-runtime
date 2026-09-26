// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift per-stream mailbox + output ring state machine.
 * @ingroup aux_util
 *
 * See u_lift_mailbox.h. No locks here: the caller serializes.
 */

#include "util/u_lift_mailbox.h"

#include <string.h>

static bool
slot_ok_in(int32_t slot)
{
	return slot >= 0 && slot < U_LIFT_INPUT_SLOTS;
}

static bool
slot_ok_out(int32_t slot)
{
	return slot >= 0 && slot < U_LIFT_RING_SIZE;
}

void
u_lift_mailbox_init(struct u_lift_mailbox *mb)
{
	memset(mb, 0, sizeof(*mb));
	mb->latest = -1;
}

bool
u_lift_mailbox_begin_submit(struct u_lift_mailbox *mb, int32_t *out_slot)
{
	int32_t pick = -1;
	for (int32_t i = 0; i < U_LIFT_INPUT_SLOTS; i++) {
		if (mb->in_state[i] == U_LIFT_IN_FREE) {
			pick = i;
			break;
		}
	}
	if (pick < 0) {
		// No free slot: overwrite the pending one — that frame never reaches
		// the module (latest wins).
		for (int32_t i = 0; i < U_LIFT_INPUT_SLOTS; i++) {
			if (mb->in_state[i] == U_LIFT_IN_PENDING) {
				pick = i;
				mb->dropped++;
				break;
			}
		}
	}
	if (pick < 0) {
		return false;
	}
	mb->in_state[pick] = U_LIFT_IN_WRITING;
	memset(&mb->in_meta[pick], 0, sizeof(mb->in_meta[pick]));
	*out_slot = pick;
	return true;
}

uint64_t
u_lift_mailbox_commit_submit(
    struct u_lift_mailbox *mb, int32_t slot, int64_t source_time, uint64_t now_ns, uint32_t width, uint32_t height)
{
	if (!slot_ok_in(slot) || mb->in_state[slot] != U_LIFT_IN_WRITING) {
		return 0;
	}
	// An OLDER pending frame is superseded by this one.
	for (int32_t i = 0; i < U_LIFT_INPUT_SLOTS; i++) {
		if (i != slot && mb->in_state[i] == U_LIFT_IN_PENDING) {
			mb->in_state[i] = U_LIFT_IN_FREE;
			mb->dropped++;
		}
	}
	struct u_lift_frame_meta *m = &mb->in_meta[slot];
	m->frame_id = ++mb->last_frame_id;
	m->source_time = source_time;
	m->submit_ns = now_ns;
	m->convert_start_ns = 0;
	m->done_ns = 0;
	m->width = width;
	m->height = height;
	mb->in_state[slot] = U_LIFT_IN_PENDING;
	mb->submitted++;
	return m->frame_id;
}

void
u_lift_mailbox_abort_submit(struct u_lift_mailbox *mb, int32_t slot)
{
	if (slot_ok_in(slot) && mb->in_state[slot] == U_LIFT_IN_WRITING) {
		mb->in_state[slot] = U_LIFT_IN_FREE;
	}
}

bool
u_lift_mailbox_has_pending(const struct u_lift_mailbox *mb)
{
	for (int32_t i = 0; i < U_LIFT_INPUT_SLOTS; i++) {
		if (mb->in_state[i] == U_LIFT_IN_PENDING) {
			return true;
		}
	}
	return false;
}

bool
u_lift_mailbox_take_pending(struct u_lift_mailbox *mb,
                            uint64_t now_ns,
                            int32_t *out_slot,
                            struct u_lift_frame_meta *out_meta)
{
	int32_t pick = -1;
	for (int32_t i = 0; i < U_LIFT_INPUT_SLOTS; i++) {
		if (mb->in_state[i] != U_LIFT_IN_PENDING) {
			continue;
		}
		if (pick < 0 || mb->in_meta[i].frame_id > mb->in_meta[pick].frame_id) {
			pick = i;
		}
	}
	if (pick < 0) {
		return false;
	}
	mb->in_state[pick] = U_LIFT_IN_CONVERTING;
	mb->in_meta[pick].convert_start_ns = now_ns;
	*out_slot = pick;
	if (out_meta != NULL) {
		*out_meta = mb->in_meta[pick];
	}
	return true;
}

void
u_lift_mailbox_finish_input(struct u_lift_mailbox *mb, int32_t slot)
{
	if (slot_ok_in(slot) && mb->in_state[slot] == U_LIFT_IN_CONVERTING) {
		mb->in_state[slot] = U_LIFT_IN_FREE;
	}
}

bool
u_lift_mailbox_begin_output(struct u_lift_mailbox *mb, int32_t *out_slot)
{
	for (int32_t i = 0; i < U_LIFT_RING_SIZE; i++) {
		if (i == mb->latest || mb->out_pins[i] > 0 || mb->out_state[i] == U_LIFT_OUT_WRITING) {
			continue;
		}
		mb->out_state[i] = U_LIFT_OUT_WRITING;
		*out_slot = i;
		return true;
	}
	return false;
}

void
u_lift_mailbox_publish_output(struct u_lift_mailbox *mb,
                              int32_t slot,
                              const struct u_lift_frame_meta *meta,
                              uint64_t now_ns)
{
	if (!slot_ok_out(slot) || mb->out_state[slot] != U_LIFT_OUT_WRITING || meta == NULL) {
		return;
	}
	struct u_lift_frame_meta *m = &mb->out_meta[slot];
	*m = *meta;
	m->done_ns = now_ns;
	mb->out_state[slot] = U_LIFT_OUT_READY;
	mb->latest = slot;
	mb->converted++;

	if (mb->last_publish_ns != 0 && now_ns > mb->last_publish_ns) {
		uint64_t iv = now_ns - mb->last_publish_ns;
		if (mb->interval_ema_ns == 0) {
			mb->interval_ema_ns = iv;
		} else if (iv >= mb->interval_ema_ns) {
			mb->interval_ema_ns += (iv - mb->interval_ema_ns) / 8;
		} else {
			mb->interval_ema_ns -= (mb->interval_ema_ns - iv) / 8;
		}
	}
	mb->last_publish_ns = now_ns;

	uint64_t lat = now_ns >= m->submit_ns ? now_ns - m->submit_ns : 0;
	mb->lat_last_ns = lat;
	if (mb->converted == 1) {
		mb->lat_min_ns = lat;
		mb->lat_max_ns = lat;
		mb->lat_ema_ns = lat;
	} else {
		if (lat < mb->lat_min_ns) {
			mb->lat_min_ns = lat;
		}
		if (lat > mb->lat_max_ns) {
			mb->lat_max_ns = lat;
		}
		// alpha = 1/8, integer: ema += (lat - ema) / 8, signed-safe.
		if (lat >= mb->lat_ema_ns) {
			mb->lat_ema_ns += (lat - mb->lat_ema_ns) / 8;
		} else {
			mb->lat_ema_ns -= (mb->lat_ema_ns - lat) / 8;
		}
	}
}

void
u_lift_mailbox_abort_output(struct u_lift_mailbox *mb, int32_t slot)
{
	if (slot_ok_out(slot) && mb->out_state[slot] == U_LIFT_OUT_WRITING) {
		mb->out_state[slot] = U_LIFT_OUT_EMPTY;
		memset(&mb->out_meta[slot], 0, sizeof(mb->out_meta[slot]));
		mb->failed++;
	}
}

bool
u_lift_mailbox_pin_latest(struct u_lift_mailbox *mb,
                          bool only_newer,
                          int32_t *out_slot,
                          struct u_lift_frame_meta *out_meta)
{
	int32_t s = mb->latest;
	if (!slot_ok_out(s) || mb->out_state[s] != U_LIFT_OUT_READY) {
		return false;
	}
	if (only_newer) {
		if (mb->out_meta[s].frame_id <= mb->last_acquired_frame_id) {
			return false;
		}
		mb->last_acquired_frame_id = mb->out_meta[s].frame_id;
	}
	mb->out_pins[s]++;
	*out_slot = s;
	if (out_meta != NULL) {
		*out_meta = mb->out_meta[s];
	}
	return true;
}

void
u_lift_mailbox_unpin(struct u_lift_mailbox *mb, int32_t slot)
{
	if (slot_ok_out(slot) && mb->out_pins[slot] > 0) {
		mb->out_pins[slot]--;
	}
}

bool
u_lift_mailbox_has_result(const struct u_lift_mailbox *mb)
{
	return slot_ok_out(mb->latest) && mb->out_state[mb->latest] == U_LIFT_OUT_READY;
}

float
u_lift_mailbox_rate_hz(const struct u_lift_mailbox *mb)
{
	return mb->interval_ema_ns > 0 ? (float)(1e9 / (double)mb->interval_ema_ns) : 0.0f;
}

void
u_lift_sched_init(struct u_lift_sched *s)
{
	memset(s, 0, sizeof(*s));
}

uint32_t
u_lift_sched_plan(
    struct u_lift_sched *s, const struct u_lift_sched_entry *entries, uint32_t count, uint64_t *out_ids, uint32_t max)
{
	uint32_t n = 0;
	bool any = false;
	for (uint32_t i = 0; i < count; i++) {
		if (entries[i].pending && entries[i].priority != U_LIFT_PRIORITY_PAUSED) {
			any = true;
		}
	}
	if (!any) {
		return 0;
	}
	const bool low_round = (s->round % U_LIFT_LOW_EVERY_N) == 0;
	s->round++;

	// Every HIGH stream with a new frame.
	for (uint32_t i = 0; i < count && n < max; i++) {
		if (entries[i].pending && entries[i].priority >= U_LIFT_PRIORITY_HIGH) {
			out_ids[n++] = entries[i].id;
		}
	}

	// ONE NORMAL stream, round-robin: the first after the last served, else wrap.
	int32_t pick = -1;
	for (uint32_t i = 0; i < count; i++) {
		if (entries[i].pending && entries[i].priority == U_LIFT_PRIORITY_NORMAL &&
		    entries[i].id > s->normal_rr_last) {
			pick = (int32_t)i;
			break;
		}
	}
	if (pick < 0) {
		for (uint32_t i = 0; i < count; i++) {
			if (entries[i].pending && entries[i].priority == U_LIFT_PRIORITY_NORMAL) {
				pick = (int32_t)i;
				break;
			}
		}
	}
	if (pick >= 0 && n < max) {
		out_ids[n++] = entries[pick].id;
		s->normal_rr_last = entries[pick].id;
	}

	// LOW: every stream with a new frame, every Nth round.
	if (low_round) {
		for (uint32_t i = 0; i < count && n < max; i++) {
			if (entries[i].pending && entries[i].priority == U_LIFT_PRIORITY_LOW) {
				out_ids[n++] = entries[i].id;
			}
		}
	}
	return n;
}


/*
 *
 * Snapshot size cap.
 *
 */

uint32_t
u_lift_max_input_edge_parse(const char *value)
{
	if (value == NULL || value[0] == '\0') {
		return U_LIFT_MAX_INPUT_EDGE_DEFAULT;
	}
	uint64_t v = 0;
	for (const char *p = value; *p != '\0'; p++) {
		if (*p < '0' || *p > '9') {
			return U_LIFT_MAX_INPUT_EDGE_DEFAULT;
		}
		v = v * 10u + (uint64_t)(*p - '0');
		if (v > 0xffffu) {
			v = 0xffffu; // far beyond any D3D11 texture edge; saturate
		}
	}
	if (v == 0) {
		return 0;
	}
	return v < U_LIFT_MAX_INPUT_EDGE_MIN ? U_LIFT_MAX_INPUT_EDGE_MIN : (uint32_t)v;
}

bool
u_lift_cap_dims(uint32_t w, uint32_t h, uint32_t cap, uint32_t *out_w, uint32_t *out_h)
{
	*out_w = w;
	*out_h = h;
	const uint32_t long_edge = w > h ? w : h;
	if (cap == 0 || long_edge <= cap || w == 0 || h == 0) {
		return false;
	}
	const uint64_t ce = (uint64_t)(cap & ~1u) < 2u ? 2u : (uint64_t)(cap & ~1u);
	const uint64_t short_edge = w > h ? h : w;
	// Nearest even: 2 * round(short * ce / (2 * long)).
	uint64_t se = 2u * ((short_edge * ce + long_edge) / (2u * (uint64_t)long_edge));
	se = se < 2u ? 2u : se;
	se = se > ce ? ce : se;
	if (w >= h) {
		*out_w = (uint32_t)ce;
		*out_h = (uint32_t)se;
	} else {
		*out_w = (uint32_t)se;
		*out_h = (uint32_t)ce;
	}
	return true;
}


/*
 *
 * Letterbox crop.
 *
 */

bool
u_lift_letterbox_parse(const char *value)
{
	return !(value != NULL && value[0] == '0' && value[1] == '\0');
}

bool
u_lift_crop_active(const struct u_lift_crop *c)
{
	return c->top != 0 || c->bottom != 0 || c->left != 0 || c->right != 0;
}


/*!
 * Bar sizes (pixels) at both ends of one axis of length @p len profiled in
 * @p n buckets. A bar is the run from an edge of buckets below @p thr.
 *
 * Symmetry: a film is centred, so when one bar measures shorter, the shorter
 * one exists, and the extra band on its side is either separated from the
 * picture by a black gap (a subtitle line, however dense) or only sparsely lit
 * throughout, the shorter bar is really as long as the other.
 */
static void
letterbox_axis(const float *prof, uint32_t n, uint32_t len, float thr, uint32_t *out_lo, uint32_t *out_hi)
{
	*out_lo = 0;
	*out_hi = 0;
	if (prof == NULL || n == 0 || len == 0) {
		return;
	}
	uint32_t lo = 0;
	while (lo < n && prof[lo] < thr) {
		lo++;
	}
	if (lo == n) {
		return; // no picture anywhere on this axis: not a measurement of bars
	}
	uint32_t hi = 0;
	while (hi < n && prof[n - 1 - hi] < thr) {
		hi++;
	}
	// idx(i) = the i-th bucket counted from the SHORT bar's edge.
	for (int side = 0; side < 2; side++) {
		uint32_t *shrt = side == 0 ? &hi : &lo;
		const uint32_t lng = side == 0 ? lo : hi;
		if (!(lng > *shrt && *shrt > 0 && n - lng > *shrt)) {
			continue;
		}
#define LB_IDX(i) (side == 0 ? n - 1 - (i) : (i))
		// The band is [*shrt, lng) from the short edge; lng - 1 touches the picture.
		bool gap = prof[LB_IDX(lng - 1)] < U_LIFT_LETTERBOX_PICTURE_FRAC;
		bool sparse = true;
		for (uint32_t i = *shrt; i < lng && sparse; i++) {
			sparse = prof[LB_IDX(i)] < U_LIFT_LETTERBOX_SPARSE_FRAC;
		}
#undef LB_IDX
		if (gap || sparse) {
			*shrt = lng;
		}
	}
	// Bucket k starts at pixel k*len/n: the bars end where the first picture
	// bucket starts, so no picture row is ever inside a bar.
	uint32_t lo_px = (uint32_t)(((uint64_t)lo * len) / n);
	uint32_t hi_px = len - (uint32_t)(((uint64_t)(n - hi) * len) / n);
	lo_px &= ~1u; // even, so a capped / halved snapshot stays aligned
	hi_px &= ~1u;
	if (lo_px < U_LIFT_LETTERBOX_MIN_BAR_PX) {
		lo_px = 0;
	}
	if (hi_px < U_LIFT_LETTERBOX_MIN_BAR_PX) {
		hi_px = 0;
	}
	if ((float)(len - lo_px - hi_px) < U_LIFT_LETTERBOX_MIN_ACTIVE_FRAC * (float)len) {
		return; // implausible (a mostly-dark frame): no crop from this frame
	}
	*out_lo = lo_px;
	*out_hi = hi_px;
}

//! Each bar of @p a is no larger than the matching bar of @p b.
static bool
crop_within(const struct u_lift_crop *a, const struct u_lift_crop *b)
{
	return a->top <= b->top && a->bottom <= b->bottom && a->left <= b->left && a->right <= b->right;
}

static uint32_t
min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static bool
near_u32(uint32_t a, uint32_t b, uint32_t tol)
{
	return a > b ? a - b <= tol : b - a <= tol;
}

static bool
crop_near(const struct u_lift_crop *a, const struct u_lift_crop *b, uint32_t tol_v, uint32_t tol_h)
{
	return near_u32(a->top, b->top, tol_v) && near_u32(a->bottom, b->bottom, tol_v) &&
	       near_u32(a->left, b->left, tol_h) && near_u32(a->right, b->right, tol_h);
}

static void
crop_min_into(struct u_lift_crop *acc, const struct u_lift_crop *m)
{
	acc->top = min_u32(acc->top, m->top);
	acc->bottom = min_u32(acc->bottom, m->bottom);
	acc->left = min_u32(acc->left, m->left);
	acc->right = min_u32(acc->right, m->right);
}

bool
u_lift_letterbox_update(struct u_lift_letterbox *lb,
                        uint32_t w,
                        uint32_t h,
                        const float *rows,
                        uint32_t nr,
                        const float *cols,
                        uint32_t nc)
{
	const struct u_lift_crop none = {0, 0, 0, 0};
	bool changed = false;
	if (lb->w != w || lb->h != h) {
		changed = u_lift_crop_active(&lb->committed);
		lb->w = w;
		lb->h = h;
		lb->committed = none;
		lb->pending = none;
		lb->pending_frames = 0;
		lb->shrink = none;
		lb->shrink_frames = 0;
	}

	// A frame with no picture on either axis (black, a fade) says nothing.
	bool content = false;
	for (uint32_t i = 0; i < nr && !content; i++) {
		content = rows[i] >= U_LIFT_LETTERBOX_PICTURE_FRAC;
	}
	if (!content) {
		return changed;
	}

	// Two readings of the same profile: bars as far as the rows are truly
	// BLACK (what the crop may grow into), and as far as no PICTURE intrudes
	// (what the crop must shrink back to).
	struct u_lift_crop grow = none, keep = none;
	letterbox_axis(rows, nr, h, U_LIFT_LETTERBOX_BLACK_FRAC, &grow.top, &grow.bottom);
	letterbox_axis(cols, nc, w, U_LIFT_LETTERBOX_BLACK_FRAC, &grow.left, &grow.right);
	letterbox_axis(rows, nr, h, U_LIFT_LETTERBOX_PICTURE_FRAC, &keep.top, &keep.bottom);
	letterbox_axis(cols, nc, w, U_LIFT_LETTERBOX_PICTURE_FRAC, &keep.left, &keep.right);

	// Profiles jitter by a bucket frame to frame: "the same" within a tolerance,
	// settling on the SMALLEST bars seen (never crop picture).
	const uint32_t tol_v = h / 64u > 8u ? h / 64u : 8u;
	const uint32_t tol_h = w / 64u > 8u ? w / 64u : 8u;

	// An intrusion within the jitter tolerance is bucket rounding (the symmetry
	// rule mirrors a bar's BUCKET count, which lands a few pixels off the other
	// bar's pixel size), not picture: it does not shrink the crop.
#define LB_RELAX(e, t)                                                                                               \
	if (lb->committed.e > keep.e && lb->committed.e - keep.e <= (t)) {                                           \
		keep.e = lb->committed.e;                                                                            \
	}
	LB_RELAX(top, tol_v)
	LB_RELAX(bottom, tol_v)
	LB_RELAX(left, tol_h)
	LB_RELAX(right, tol_h)
#undef LB_RELAX

	// Shrink: picture inside a committed bar, held for SHRINK_FRAMES frames.
	if (!crop_within(&lb->committed, &keep)) {
		struct u_lift_crop target = lb->committed;
		crop_min_into(&target, &keep);
		if (lb->shrink_frames > 0 && crop_near(&target, &lb->shrink, tol_v, tol_h)) {
			crop_min_into(&lb->shrink, &target);
			lb->shrink_frames++;
		} else {
			lb->shrink = target;
			lb->shrink_frames = 1;
		}
		if (lb->shrink_frames >= U_LIFT_LETTERBOX_SHRINK_FRAMES) {
			lb->committed = lb->shrink;
			lb->shrink_frames = 0;
			lb->pending = lb->committed;
			lb->pending_frames = 0;
			return true;
		}
		return changed; // no growth while picture is intruding
	}
	lb->shrink_frames = 0;

	// Grow: larger truly-black bars have to settle first.
	if (crop_within(&grow, &lb->committed)) {
		lb->pending = lb->committed;
		lb->pending_frames = 0;
		return changed;
	}
	if (lb->pending_frames > 0 && crop_near(&grow, &lb->pending, tol_v, tol_h)) {
		crop_min_into(&lb->pending, &grow);
		lb->pending_frames++;
	} else {
		lb->pending = grow;
		lb->pending_frames = 1;
	}
	if (lb->pending_frames >= U_LIFT_LETTERBOX_SETTLE_FRAMES) {
		// Never grow a bar beyond what the no-picture reading allows either.
		struct u_lift_crop next = lb->pending;
		crop_min_into(&next, &keep);
		// Deadband: once a crop is in effect, growing it by a few pixels buys
		// nothing (the recompose's flat bars already reach into the active
		// area) and would re-size the module's input every settle period.
		const uint32_t db_v = h / 256u > 8u ? h / 256u : 8u;
		const uint32_t db_h = w / 256u > 8u ? w / 256u : 8u;
		const struct u_lift_crop *c = &lb->committed;
		const bool worth = !u_lift_crop_active(c) || next.top > c->top + db_v || next.bottom > c->bottom + db_v ||
		                   next.left > c->left + db_h || next.right > c->right + db_h;
		if (worth && memcmp(&next, &lb->committed, sizeof(next)) != 0) {
			lb->committed = next;
			changed = true;
		}
		lb->pending_frames = 0;
	}
	return changed;
}
