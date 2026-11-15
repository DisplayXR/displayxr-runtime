// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Is any pixel of a 3D window actually visible? A low-rate,
 *         vendor-neutral answer for the runtime's lens wish (#2671).
 * @ingroup aux_util
 *
 * ## Why this exists
 *
 * On a switchable-lens panel the lens is panel-wide. Before #2671 every lens
 * owner in the runtime wished "3D" for as long as a session rendered, so a
 * windowed 3D app that was minimized, or fully covered by ordinary 2D windows,
 * kept the whole panel in 3D and showed those 2D windows through an active
 * lens. A display processor that lets its client route the weave has no window
 * test of its own on that path: the client owns the lens, so the client (the
 * runtime) has to know whether its window can be seen.
 *
 * ## The policy (ADR-062 D1)
 *
 * A 3D window is **hidden** when its top-level window is minimized, hidden
 * after it was once shown, or cloaked; when the session is locked or the
 * console display is off; or when the part of its client rect on the 3D
 * screen is covered by opaque top-level windows above it in z-order except
 * for less than @ref U_WINDOW_VISIBILITY_MIN_VISIBLE_PERMILLE of its area (or
 * @ref U_WINDOW_VISIBILITY_MIN_VISIBLE_PX2, whichever is larger), so a sliver
 * behind the taskbar or a window edge does not hold the panel. A window that
 * has **never been shown** is @ref U_WV_REASON_NOT_SHOWN and never hidden: an
 * app that renders its first frame into a hidden window and then shows it
 * must not be demoted at startup.
 *
 * Timing: minimize, hide, cloak, lock and display-off are reported at once;
 * covered after @ref U_WINDOW_VISIBILITY_HIDE_DWELL_NS, and never while a
 * window is being moved or sized (the dwell restarts when the drag ends).
 * Visible is reported at once. Window events (foreground, minimize, show,
 * hide, cloak, move) drive an immediate re-check; a slow safety poll covers
 * the rest and does not speed up while hidden.
 *
 * The lens owner wishes 2D only while every one of its windows is hidden,
 * and restores the session's own choice (never a forced 3D) when one comes
 * back, per docs/specs/vendor/lens-preference-ownership.md. Tier 2 (ADR-062
 * D3) reads @ref u_window_visibility_hidden_everywhere: hidden on every
 * screen, 2D ones included.
 *
 * ## Cost
 *
 * The check runs on the tracker's own thread, on window events and a slow
 * poll, never on a render thread and never under a compositor lock. Readers
 * get one atomic load (@ref u_window_visibility_hidden), cheap enough for
 * every frame.
 *
 * Occluder rules (Windows), each of which removes a real false positive:
 * - only windows above the target's root window in z-order count;
 * - the target is the client rect clipped to the 3D screen, occluders are
 *   their DWM extended frame bounds (so drop shadows do not count), read only
 *   for windows whose plain window rect intersects (a cheap pre-filter);
 * - the taskbar occludes unless it is set to auto-hide;
 * - cloaked windows, shell and compositor overlay windows, known overlay
 *   processes (configurable, see @ref U_WINDOW_VISIBILITY_ENV_OVERLAYS),
 *   windows of the target's own process or of this process, and click-through
 *   or non-opaque layered windows never occlude.
 *
 * ## Other 3D screens (ADR-047 multi-screen)
 *
 * A window that spans screens has one segment DP per extra 3D screen, and each
 * screen's lens is its own. @ref u_window_visibility_set_screens names those
 * screens; the tracker then judges the window's part on each one separately
 * (same occluders, same dwell) and @ref u_window_visibility_screen_hidden
 * answers per screen. A screen the window has no part on is never "hidden":
 * the segment coverage gate (#2247) owns that case. A minimized, invisible or
 * cloaked window is hidden on every screen.
 *
 * Other platforms get a tracker that never reports hidden (follow-up: macOS
 * occlusion state, X11/Wayland visibility, Android surface lifecycle).
 *
 * `DXR_LENS_VISIBILITY=0` turns the policy off (create returns NULL). It,
 * @ref U_WINDOW_VISIBILITY_ENV_OVERLAYS and
 * @ref U_WINDOW_VISIBILITY_ENV_SKIP_PRESENT are persisted settings
 * (util/u_setting.h, `displayxr-cli perf set`), so the service reads them too.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! A covered window must stay covered this long before the tracker reports it hidden.
#define U_WINDOW_VISIBILITY_HIDE_DWELL_NS (1500ull * 1000ull * 1000ull)

//! Safety-poll period, visible or hidden alike (events do the fast work).
#define U_WINDOW_VISIBILITY_PERIOD_MS 500u

//! Re-check this soon after a window event (lets the window settle).
#define U_WINDOW_VISIBILITY_EVENT_DELAY_MS 30u

//! Re-check this soon after a window moved (location events are coalesced).
#define U_WINDOW_VISIBILITY_MOVE_DELAY_MS 150u

//! A move/size loop with no end event is treated as over after this long.
#define U_WINDOW_VISIBILITY_MOVESIZE_TIMEOUT_NS (10ull * 1000ull * 1000ull * 1000ull)

//! Less than this share (per mille) of the window's area left visible is "covered".
#define U_WINDOW_VISIBILITY_MIN_VISIBLE_PERMILLE 20u

//! ...or less than this many pixels, whichever is larger.
#define U_WINDOW_VISIBILITY_MIN_VISIBLE_PX2 4096ull

//! Setting (env or persisted): extra overlay process image names (comma-separated,
//! e.g. `foo.exe,bar.exe`) that never count as occluders, added to the built-in list.
#define U_WINDOW_VISIBILITY_ENV_OVERLAYS "DXR_LENS_OVERLAY_PROCESSES"

//! Setting (env or persisted): `0` turns the visibility policy off entirely.
#define U_WINDOW_VISIBILITY_ENV_ENABLE "DXR_LENS_VISIBILITY"

/*!
 * Setting (env or persisted): `0` turns ADR-062 tier 2 off, so a window
 * hidden on every screen keeps being woven and presented. See
 * @ref u_window_visibility_skip_present_enabled.
 */
#define U_WINDOW_VISIBILITY_ENV_SKIP_PRESENT "DXR_HIDDEN_SKIP_PRESENT"

/*
 *
 * Pure core: no OS calls, unit-tested (tests/tests_aux_window_visibility.cpp).
 *
 */

//! A half-open screen rect: [left, right) x [top, bottom).
struct u_wv_rect
{
	int32_t left;
	int32_t top;
	int32_t right;
	int32_t bottom;
};

//! True when @p r has no area.
static inline bool
u_wv_rect_empty(const struct u_wv_rect *r)
{
	return r->right <= r->left || r->bottom <= r->top;
}

//! Intersection of @p a and @p b (may be empty).
static inline struct u_wv_rect
u_wv_rect_intersect(const struct u_wv_rect *a, const struct u_wv_rect *b)
{
	struct u_wv_rect r;
	r.left = a->left > b->left ? a->left : b->left;
	r.top = a->top > b->top ? a->top : b->top;
	r.right = a->right < b->right ? a->right : b->right;
	r.bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
	return r;
}

/*!
 * True when the union of @p occluders covers every pixel of @p target.
 *
 * Rect subtraction with a bounded work list. If the uncovered remainder ever
 * fragments past the work list's capacity the answer is false (visible): the
 * conservative side, which keeps the lens in 3D.
 *
 * An empty @p target is reported covered=false: there is nothing to judge.
 */
bool
u_wv_rect_covered(const struct u_wv_rect *target, const struct u_wv_rect *occluders, size_t occluder_count);

/*!
 * Why the window is (or is not) hidden. The hidden reasons are the ones
 * @ref u_wv_reason_is_hidden accepts; the others describe a window that is
 * not hidden.
 */
enum u_wv_reason
{
	U_WV_REASON_VISIBLE = 0,    //!< a meaningful part can be seen, nothing over it
	U_WV_REASON_PARTLY_COVERED, //!< visible, some occluder over it
	U_WV_REASON_NOT_SHOWN,      //!< never shown yet: not hidden (startup)
	U_WV_REASON_OFF_SCREEN,     //!< no part on the 3D screen: not hidden here
	U_WV_REASON_GONE,           //!< the window no longer exists: not hidden
	U_WV_REASON_MINIMIZED,      //!< hidden at once
	U_WV_REASON_NOT_VISIBLE,    //!< hidden after it was once shown: hidden at once
	U_WV_REASON_CLOAKED,        //!< other virtual desktop and the like: hidden at once
	U_WV_REASON_LOCKED,         //!< the session is locked: hidden at once
	U_WV_REASON_DISPLAY_OFF,    //!< the console display is off: hidden at once
	U_WV_REASON_COVERED,        //!< covered by other windows: hidden after the dwell
};

//! Short lowercase label, e.g. "covered", "minimized", "not-shown".
const char *
u_wv_reason_str(enum u_wv_reason r);

//! True for the reasons that make a window hidden.
static inline bool
u_wv_reason_is_hidden(enum u_wv_reason r)
{
	return r >= U_WV_REASON_MINIMIZED;
}

//! True for the hidden reasons reported without a dwell (all but COVERED).
static inline bool
u_wv_reason_is_immediate(enum u_wv_reason r)
{
	return u_wv_reason_is_hidden(r) && r != U_WV_REASON_COVERED;
}

//! What the platform layer knows about the target window and the session.
struct u_wv_window_state
{
	bool exists;      //!< the window handle is valid
	bool ever_shown;  //!< it has been visible at least once since tracking began
	bool visible;     //!< visible now (it and its root window)
	bool minimized;   //!< its root window is minimized
	bool cloaked;     //!< its root window is cloaked
	bool locked;      //!< the session is locked
	bool display_off; //!< the console display is off
};

/*!
 * The window-level verdict before any cover test: a hidden reason,
 * @ref U_WV_REASON_NOT_SHOWN or @ref U_WV_REASON_GONE, or
 * @ref U_WV_REASON_VISIBLE meaning "go on and test the cover". Pure.
 */
enum u_wv_reason
u_wv_window_reason(const struct u_wv_window_state *s);

/*!
 * Area of @p target left uncovered by @p occluders, in pixels, through
 * @p out_area. Returns false when the remainder fragments past the work list
 * (the caller must then treat the target as visible).
 */
bool
u_wv_rect_uncovered_area(const struct u_wv_rect *target,
                         const struct u_wv_rect *occluders,
                         size_t occluder_count,
                         uint64_t *out_area);

/*!
 * The least visible area (pixels) that keeps a part of @p part_area pixels
 * visible: the larger of @ref U_WINDOW_VISIBILITY_MIN_VISIBLE_PERMILLE of it
 * and @ref U_WINDOW_VISIBILITY_MIN_VISIBLE_PX2, never more than the part
 * itself (so an uncovered small window is always visible).
 */
static inline uint64_t
u_wv_min_visible_area(uint64_t part_area)
{
	uint64_t t = part_area * U_WINDOW_VISIBILITY_MIN_VISIBLE_PERMILLE / 1000u;
	if (t < U_WINDOW_VISIBILITY_MIN_VISIBLE_PX2) {
		t = U_WINDOW_VISIBILITY_MIN_VISIBLE_PX2;
	}
	return t < part_area ? t : part_area;
}

/*!
 * True when @p target is covered by @p occluders except for less than
 * @ref u_wv_min_visible_area of it. Empty target: false. Too fragmented to
 * tell: false (visible, the conservative side).
 */
bool
u_wv_rect_mostly_covered(const struct u_wv_rect *target, const struct u_wv_rect *occluders, size_t occluder_count);

//! Facts about one candidate occluder window, gathered by the platform layer.
struct u_wv_window_facts
{
	//! Window class name, UTF-8 (may be empty).
	const char *class_name;
	//! Process image base name, UTF-8, e.g. `explorer.exe` (may be empty).
	const char *process_name;
	//! Owned by the target window's process, or by this process.
	bool same_process;
	//! Visible (and not minimized).
	bool visible;
	//! Cloaked by the desktop compositor (other virtual desktop, suspended frame...).
	bool cloaked;
	//! Click-through (input-transparent) window.
	bool input_transparent;
	//! Layered window that is not known to be fully opaque.
	bool layered_not_opaque;
	//! Frame class whose content is rendered without a redirection surface.
	bool no_redirection_bitmap;
	//! The taskbar is set to auto-hide (a taskbar then never occludes; else it does).
	bool taskbar_auto_hide;
};

//! Why a window does not occlude; U_WV_OCCLUDES when it does.
enum u_wv_skip
{
	U_WV_OCCLUDES = 0,
	U_WV_SKIP_NOT_VISIBLE,
	U_WV_SKIP_CLOAKED,
	U_WV_SKIP_SAME_PROCESS,
	U_WV_SKIP_TRANSPARENT,
	U_WV_SKIP_OVERLAY_CLASS,
	U_WV_SKIP_OVERLAY_PROCESS,
};

/*!
 * Classify one candidate occluder.
 *
 * @param extra_overlay_processes Optional comma-separated extra process names
 *        (case-insensitive) that never occlude; NULL or "" for none.
 */
enum u_wv_skip
u_wv_classify_occluder(const struct u_wv_window_facts *f, const char *extra_overlay_processes);

//! Where the window's client rect stands on one screen (raw, before the dwell).
enum u_wv_part
{
	U_WV_PART_OFF = 0, //!< no pixel of the window is on this screen
	U_WV_PART_VISIBLE, //!< some pixel of the window's part on this screen can be seen
	U_WV_PART_HIDDEN,  //!< the window's part on this screen is entirely covered
};

/*!
 * Judge the window's part on one screen: @p client (the client rect, screen
 * pixels) clipped to @p screen, against @p occluders (any rects; only their
 * overlap with the part counts). Pure; the tracker's per-screen verdict.
 */
enum u_wv_part
u_wv_part_state(const struct u_wv_rect *client,
                const struct u_wv_rect *screen,
                const struct u_wv_rect *occluders,
                size_t occluder_count);

//! Hide-dwell / instant-show debounce.
struct u_wv_debounce
{
	//! The state readers see.
	bool reported_hidden;
	//! The raw check said hidden at the last update.
	bool raw_hidden;
	//! When the current raw-hidden run started.
	uint64_t hidden_since_ns;
};

/*!
 * Feed one raw check result. Returns the state to report: hidden only after
 * @p raw_hidden has held for @p dwell_ns, visible at once.
 */
static inline bool
u_wv_debounce_update(struct u_wv_debounce *d, bool raw_hidden, uint64_t now_ns, uint64_t dwell_ns)
{
	if (!raw_hidden) {
		d->raw_hidden = false;
		d->reported_hidden = false;
		return false;
	}
	if (!d->raw_hidden) {
		d->raw_hidden = true;
		d->hidden_since_ns = now_ns;
	}
	if (!d->reported_hidden && now_ns - d->hidden_since_ns >= dwell_ns) {
		d->reported_hidden = true;
	}
	return d->reported_hidden;
}

/*!
 * ADR-062 D1 timing. Feed one raw check: an @p immediate @p raw_hidden
 * (minimize, hide, cloak, lock, display-off) is reported hidden at once; a
 * covered one after @p dwell_ns, and while @p hold (a window is being moved
 * or sized) the dwell does not run, it restarts. Visible at once.
 */
static inline bool
u_wv_debounce_step(
    struct u_wv_debounce *d, bool raw_hidden, bool immediate, bool hold, uint64_t now_ns, uint64_t dwell_ns)
{
	if (!raw_hidden) {
		d->raw_hidden = false;
		d->reported_hidden = false;
		return false;
	}
	if (!d->raw_hidden || (hold && !immediate && !d->reported_hidden)) {
		d->raw_hidden = true;
		d->hidden_since_ns = now_ns;
	}
	if (!d->reported_hidden && (immediate || (!hold && now_ns - d->hidden_since_ns >= dwell_ns))) {
		d->reported_hidden = true;
	}
	return d->reported_hidden;
}

/*!
 * When a covered run not yet reported would cross the dwell (absolute ns on
 * the same clock), or 0 when nothing is pending. The tracker wakes then.
 */
static inline uint64_t
u_wv_debounce_deadline(const struct u_wv_debounce *d, uint64_t dwell_ns)
{
	return (d->raw_hidden && !d->reported_hidden) ? d->hidden_since_ns + dwell_ns : 0;
}

/*
 *
 * ADR-062 D5 (tier 4): the visible region, and the published wish built on it.
 * Pure: no OS calls, unit-tested (tests/tests_aux_window_visibility.cpp).
 *
 */

//! Most rects one visible region holds; past it the region falls back to the whole target.
#define U_WINDOW_VISIBILITY_REGION_MAX_RECTS 32

/*!
 * A region: disjoint rects (as produced by @ref u_wv_region_compute; a merge
 * may overlap them, which a raster does not mind). When @ref whole is set the
 * region fragmented past @ref U_WINDOW_VISIBILITY_REGION_MAX_RECTS and holds
 * the whole target instead: the conservative side, which keeps the panel 3D.
 */
struct u_wv_region
{
	uint32_t count;
	bool whole;
	struct u_wv_rect rects[U_WINDOW_VISIBILITY_REGION_MAX_RECTS];
};

/*!
 * The part of @p target no occluder covers, as disjoint rects in @p out.
 * Returns false when it fragments past the cap (or the work list): @p out then
 * holds @p target alone with `whole` set. An empty target gives an empty region.
 */
bool
u_wv_region_compute(const struct u_wv_rect *target,
                    const struct u_wv_rect *occluders,
                    size_t occluder_count,
                    struct u_wv_region *out);

//! Total area of @p r's rects (overlaps counted twice; computed regions are disjoint).
uint64_t
u_wv_region_area(const struct u_wv_region *r);

//! True when every pixel of @p inner is in @p outer. Too fragmented to tell: false.
bool
u_wv_region_contains(const struct u_wv_region *outer, const struct u_wv_region *inner);

//! Same rects in the same order (and the same `whole` flag).
bool
u_wv_region_equal(const struct u_wv_region *a, const struct u_wv_region *b);

/*!
 * @p dst grows by every rect of @p add that @p dst does not already contain.
 * Past the cap @p dst becomes @p whole alone (`whole` set).
 */
void
u_wv_region_merge(struct u_wv_region *dst, const struct u_wv_region *add, const struct u_wv_rect *whole);

//! Shift every rect by (@p dx, @p dy).
void
u_wv_region_translate(struct u_wv_region *r, int32_t dx, int32_t dy);

//! @p in clipped to @p clip, empty pieces dropped (in place allowed).
void
u_wv_region_clip(const struct u_wv_region *in, const struct u_wv_rect *clip, struct u_wv_region *out);

/*!
 * The D1 timing applied to a region. A region that only grows is published at
 * once (visible is reported at once); one that loses pixels keeps the lost
 * pixels for @p dwell_ns, and while @p hold (a window is moved or sized) the
 * dwell restarts, so a window dragged across does not flip cells. Pixels that
 * appear during a dwell are added at once. @p immediate (minimize, cloak,
 * lock...) publishes @p raw at once.
 */
struct u_wv_region_debounce
{
	bool init;
	struct u_wv_region pub;     //!< what readers see
	struct u_wv_region pending; //!< the shrunk region waiting out the dwell
	bool pending_active;
	uint64_t since_ns;
};

/*!
 * Feed one raw region (@p whole: the target, for the merge fallback). Returns
 * true when the published region changed.
 */
bool
u_wv_region_debounce_step(struct u_wv_region_debounce *d,
                          const struct u_wv_region *raw,
                          bool immediate,
                          bool hold,
                          uint64_t now_ns,
                          uint64_t dwell_ns,
                          const struct u_wv_rect *whole);

//! When a pending shrink would be published (absolute ns), or 0.
static inline uint64_t
u_wv_region_debounce_deadline(const struct u_wv_region_debounce *d, uint64_t dwell_ns)
{
	return d->pending_active ? d->since_ns + dwell_ns : 0;
}

/*!
 * A window's visible region as the tracker publishes it
 * (@ref u_window_visibility_get_region), in WINDOW-CLIENT pixels of a client
 * area @ref client_w x @ref client_h (physical pixels as the tracker measured
 * them; a consumer scales to its own mask by these dims).
 */
struct u_window_visibility_region
{
	//! Content generation: bumps when anything below changes. 0 = nothing published yet.
	uint64_t gen;
	//! False: nothing is known (not shown yet, gone, untracked) - treat the whole window as visible.
	bool known;
	uint32_t client_w;
	uint32_t client_h;
	struct u_wv_region region;
};

/*!
 * True when the tier-4 region mask applies to a display processor whose zone
 * caps report a @p grid_w x @p grid_h cell grid: only a grid larger than 1x1.
 * A 1x1 (panel-wide) switch keeps the tier-1 boolean lens path, unchanged.
 */
static inline bool
u_wv_region_mask_applies(uint32_t grid_w, uint32_t grid_h)
{
	return (uint64_t)grid_w * (uint64_t)grid_h > 1u;
}

//! What a tier-4 publish is built from.
enum u_wv_wish_kind
{
	U_WV_WISH_EMPTY = 0, //!< nothing 3D: the window is hidden or the session is in 2D mode
	U_WV_WISH_CUT,       //!< an existing wish mask (zones / sticky), its covered pixels cleared
	U_WV_WISH_IMPLICIT,  //!< no wish of the app's own: the visible client region, minus flat rects
};

//! Most rects a wish plan carries in each list.
#define U_WV_WISH_MAX_RECTS 64

//! The inputs of @ref u_wv_wish_plan_build.
struct u_wv_wish_input
{
	enum u_wv_wish_kind kind;
	//! The published mask's dims (the CUT source's, or the IMPLICIT raster's).
	uint32_t mask_w;
	uint32_t mask_h;
	//! CUT: the source mask's content generation (pixels the plan cannot see).
	uint64_t source_seq;
	//! The visible region; NULL or not known = the whole window is visible.
	const struct u_window_visibility_region *visible;
	//! IMPLICIT: rects that stay flat (Local2D layer rects), in client pixels of
	//! a @ref region_w x @ref region_h client area.
	const struct u_wv_rect *flat;
	uint32_t flat_count;
	uint32_t region_w;
	uint32_t region_h;
};

/*!
 * A plan in MASK pixels: start from the source (CUT) or from all-zero
 * (EMPTY, IMPLICIT), set @ref on to 1, then set @ref off to 0. Rounding errs
 * toward 3D: on-rects expand outward, off-rects shrink inward.
 */
struct u_wv_wish_plan
{
	enum u_wv_wish_kind kind;
	uint32_t mask_w;
	uint32_t mask_h;
	uint32_t on_count;
	struct u_wv_rect on[U_WV_WISH_MAX_RECTS];
	uint32_t off_count;
	struct u_wv_rect off[U_WV_WISH_MAX_RECTS];
	//! CUT: the covered part fragmented past the cap, so the source is published uncut (the 3D side).
	bool cut_skipped;
	//! Content signature: equal plans (and equal sources) sign equal.
	uint64_t signature;
};

//! Build the plan for @p in. Pure.
void
u_wv_wish_plan_build(const struct u_wv_wish_input *in, struct u_wv_wish_plan *out);

//! True when the plan's mask has no 3D pixel by construction (EMPTY, or IMPLICIT with nothing on).
static inline bool
u_wv_wish_plan_is_empty(const struct u_wv_wish_plan *p)
{
	return p->kind == U_WV_WISH_EMPTY || (p->kind == U_WV_WISH_IMPLICIT && p->on_count == 0);
}

/*!
 * The #2677 publish rule: republish every frame (the screen anchor moves), but
 * bump the content generation only when the content changed. Zero-init.
 */
struct u_wv_wish_seq
{
	bool init;
	uint64_t signature;
	uint64_t seq;
};

//! The generation to publish @p signature under; bumps (and sets @p out_changed) on a change.
static inline uint64_t
u_wv_wish_seq_step(struct u_wv_wish_seq *s, uint64_t signature, bool *out_changed)
{
	const bool changed = !s->init || s->signature != signature;
	if (changed) {
		s->init = true;
		s->signature = signature;
		s->seq++;
	}
	if (out_changed != NULL) {
		*out_changed = changed;
	}
	return s->seq;
}

/*
 *
 * The tracker.
 *
 */

struct u_window_visibility;

/*!
 * Called on the tracker's thread when the debounced state changes (and never
 * otherwise). The owner applies its lens edge here: a minimized app may stop
 * submitting frames, so a check that ran only on the weave path could miss
 * the edge. Take only the locks the owner's app-thread request path
 * (xrRequestDisplayModeDXR) already takes; never block on a render.
 */
typedef void (*u_window_visibility_change_fn)(void *userdata, bool hidden);

/*!
 * Start tracking @p native_window (Windows: an HWND; the tracker walks from
 * its root window). @p panel_device_name names the 3D screen (Windows: the GDI
 * device name, `xrt_system_compositor_info::display_device_name`); NULL or ""
 * means unknown, and the target is then the whole client rect. @p who labels
 * the transition log lines.
 *
 * Returns NULL when @p native_window is NULL, the platform has no tracker, or
 * the policy is turned off — callers treat NULL as "always visible".
 *
 * @p on_change (optional) runs on the tracker thread at each transition.
 *
 * The tracker owns one thread, joined by @ref u_window_visibility_destroy.
 * Destroy it before anything @p on_change touches, and never while holding a
 * lock @p on_change takes (the join would wait on it).
 */
struct u_window_visibility *
u_window_visibility_create(void *native_window,
                           const char *panel_device_name,
                           const char *who,
                           u_window_visibility_change_fn on_change,
                           void *userdata);

/*!
 * Debounced result: true while the window has been hidden for the dwell.
 * NULL-safe (false). One atomic load; any thread.
 */
bool
u_window_visibility_hidden(const struct u_window_visibility *wv);

/*!
 * ADR-062 D3 (tier 2): the window is hidden on **every** screen it covers,
 * 2D screens included (debounced like @ref u_window_visibility_hidden). The
 * compositor may then skip its weave and present. False for a NULL tracker,
 * or when the tier-2 setting is off. One atomic load; any thread.
 */
bool
u_window_visibility_hidden_everywhere(const struct u_window_visibility *wv);

/*!
 * The tier-2 switch (@ref U_WINDOW_VISIBILITY_ENV_SKIP_PRESENT, default on),
 * latched once per process. Exposed for the compositors' create-time logs.
 */
bool
u_window_visibility_skip_present_enabled(void);

/*!
 * ADR-062 D3 (tier 2) bookkeeping for one weave path: edge logs, the count of
 * skipped frames, and the pacing grid of skipped app frames. Zero-init; owned
 * by the compositor, under the lock its frame path already holds.
 */
struct u_window_visibility_skip
{
	bool hidden;       //!< the last step skipped
	uint64_t skipped;  //!< frames skipped since the hidden edge
	uint64_t since_ns; //!< when the hidden edge was seen
	uint64_t next_ns;  //!< pacing grid of skipped app frames (0 = none yet)
};

/*!
 * One frame's tier-2 verdict: true when @p wv reports the window hidden on
 * every screen (and tier 2 is on), so the caller skips its weave and present
 * and keeps everything alive. One WARN per edge, labelled @p who; the visible
 * edge's names the frames skipped and the time hidden. With a nonzero
 * @p pace_period_ns a skipped frame is slept onto that period grid, so an app
 * frame that presents nothing is paced as a presented one would be (pass 0
 * from a repaint loop, or from a tier with its own unpresented pacing).
 */
bool
u_window_visibility_skip_step(struct u_window_visibility_skip *k,
                              const struct u_window_visibility *wv,
                              const char *who,
                              uint64_t pace_period_ns);

//! Longest covering-process name carried in @ref u_window_visibility_state.
#define U_WINDOW_VISIBILITY_NAME_MAX 64

//! A snapshot for status and logs (@ref u_window_visibility_get_state).
struct u_window_visibility_state
{
	bool tracked;           //!< a live tracker answered (false: everything below is zero)
	bool hidden;            //!< debounced, the session's 3D screen
	bool hidden_everywhere; //!< debounced, every screen (tier 2)
	enum u_wv_reason reason; //!< why, as of the last check
	//! The process owning the largest occluder of the last check ("" = none).
	char occluder[U_WINDOW_VISIBILITY_NAME_MAX];
	uint64_t since_ns; //!< monotonic time of the last debounced transition (0 = none yet)
};

/*!
 * Fill @p out (zeroed first). NULL-safe: a NULL tracker reports
 * `tracked = false`. Any thread; a short shared lock.
 */
void
u_window_visibility_get_state(const struct u_window_visibility *wv, struct u_window_visibility_state *out);

/*!
 * Change the 3D screen (a hotplug or a re-resolved panel). Any thread.
 */
void
u_window_visibility_set_panel(struct u_window_visibility *wv, const char *panel_device_name);

/*!
 * Track a different window (e.g. the service's presenter changed), or none
 * (NULL: the tracker idles and reports visible). The new window starts
 * visible; a pending "hidden" is cleared at once. Any thread, never blocks.
 */
void
u_window_visibility_set_window(struct u_window_visibility *wv, void *native_window);

//! Most extra 3D screens one tracker judges (@ref u_window_visibility_set_screens).
#define U_WINDOW_VISIBILITY_MAX_SCREENS 8

/*!
 * Name the 3D screens (Windows: GDI device names, `xrt_screen::device_name`)
 * whose segment DPs take a lens wish of their own; the window's part on each
 * is judged separately. Replaces the previous list (an unchanged list is a
 * no-op); every listed screen starts visible. Count 0 (or NULL) clears it.
 * Names past @ref U_WINDOW_VISIBILITY_MAX_SCREENS are ignored (they read
 * visible). Any thread; never waits on a check.
 */
void
u_window_visibility_set_screens(struct u_window_visibility *wv, const char *const *device_names, uint32_t count);

/*!
 * Debounced: the window's part on screen @p device_name has been hidden for
 * the dwell (or the whole window is minimized, invisible or cloaked). False
 * for an untracked screen, a screen the window is not on, or a NULL tracker:
 * the safe direction. Any thread; a shared lock and a short scan, cheap enough
 * for every weave.
 */
bool
u_window_visibility_screen_hidden(const struct u_window_visibility *wv, const char *device_name);

/*!
 * ADR-062 D5 (tier 4): the window's visible region (the uncovered remainder
 * after the same occluder rules, on the same thread and at the same rate as
 * the hidden verdict). @p device_name NULL: the whole window (every screen,
 * the client rect clipped to the desktop); else the window's part on that
 * screen (the session's 3D screen or one named by
 * @ref u_window_visibility_set_screens), empty while that screen's part is
 * hidden. A shrink waits out the hide dwell; growth is at once
 * (@ref u_wv_region_debounce).
 *
 * Returns false (and @p out->known false) for a NULL tracker, a platform
 * without one, or an unknown screen: the caller treats the whole window as
 * visible. Any thread: a short shared lock and a copy of a few hundred bytes.
 */
bool
u_window_visibility_get_region(const struct u_window_visibility *wv,
                               const char *device_name,
                               struct u_window_visibility_region *out);

/*!
 * The region's content generation (@ref u_window_visibility_region::gen)
 * without the copy, so a per-frame reader can skip @ref
 * u_window_visibility_get_region when nothing moved. 0 for a NULL tracker.
 * One atomic load.
 */
uint64_t
u_window_visibility_region_gen(const struct u_window_visibility *wv);

struct xrt_screen_list;

/*!
 * @ref u_window_visibility_set_screens from a screen list: every screen's
 * `device_name`, in list order (the session's own 3D screen may be among them;
 * it is judged the same way). NULL-safe.
 */
void
u_window_visibility_set_screen_list(struct u_window_visibility *wv, const struct xrt_screen_list *list);

/*!
 * Stop and join the tracker's thread, but keep the object: every reader (a
 * segment manager or hub that borrowed the pointer) keeps working and reads
 * "visible" from then on, and no transition callback runs again. For a
 * teardown that must silence the callback before the objects it touches go,
 * while borrowers still hold the pointer; free it later with
 * @ref u_window_visibility_destroy. Idempotent, NULL-safe; never call it
 * holding a lock the callback takes.
 */
void
u_window_visibility_stop(struct u_window_visibility *wv);

//! Stop and join the thread, free, and NULL the pointer. NULL-safe.
void
u_window_visibility_destroy(struct u_window_visibility **wv_ptr);

#ifdef __cplusplus
}
#endif
