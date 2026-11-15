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
 * ## The policy
 *
 * A 3D window is **hidden** when its top-level window is minimized, not
 * visible or cloaked, or when the part of its client rect on the 3D screen is
 * entirely covered by opaque top-level windows above it in z-order. Partial
 * overlap keeps it visible. Hidden is reported only after it has held for
 * @ref U_WINDOW_VISIBILITY_HIDE_DWELL_NS; visible is reported at the next
 * check. The lens owner wishes 2D only while every one of its windows is
 * hidden, and restores the session's own choice (never a forced 3D) when one
 * comes back, per docs/specs/vendor/lens-preference-ownership.md.
 *
 * ## Cost
 *
 * The check runs on the tracker's own thread at a few Hz, never on a render
 * thread and never under a compositor lock. Readers get one atomic load
 * (@ref u_window_visibility_hidden), cheap enough for every frame.
 *
 * Occluder rules (Windows), each of which removes a real false positive:
 * - only windows above the target's root window in z-order count;
 * - the target is the client rect clipped to the 3D screen, occluders are
 *   their DWM extended frame bounds (so drop shadows do not count);
 * - cloaked windows, shell and compositor overlay windows, known overlay
 *   processes (configurable, see @ref U_WINDOW_VISIBILITY_ENV_OVERLAYS),
 *   windows of the target's own process or of this process, and click-through
 *   or non-opaque layered windows never occlude.
 *
 * Other platforms get a tracker that never reports hidden (follow-up: macOS
 * occlusion state, X11/Wayland visibility, Android surface lifecycle).
 *
 * `DXR_LENS_VISIBILITY=0` turns the policy off (create returns NULL).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! A window must stay hidden this long before the tracker reports it hidden.
#define U_WINDOW_VISIBILITY_HIDE_DWELL_NS (500ull * 1000ull * 1000ull)

//! Check period while the window is visible.
#define U_WINDOW_VISIBILITY_PERIOD_VISIBLE_MS 200u

//! Check period while the window is hidden or going hidden (fast return to 3D).
#define U_WINDOW_VISIBILITY_PERIOD_HIDDEN_MS 100u

//! Env var: extra overlay process image names (comma-separated, e.g.
//! `foo.exe,bar.exe`) that never count as occluders, added to the built-in list.
#define U_WINDOW_VISIBILITY_ENV_OVERLAYS "DXR_LENS_OVERLAY_PROCESSES"

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

//! Stop and join the thread, free, and NULL the pointer. NULL-safe.
void
u_window_visibility_destroy(struct u_window_visibility **wv_ptr);

#ifdef __cplusplus
}
#endif
