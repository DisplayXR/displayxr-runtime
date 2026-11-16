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
 * Desktop Linux (X11 and XWayland, u_window_visibility_x11.c): unmapped,
 * minimized (`WM_STATE` Iconic / `_NET_WM_STATE_HIDDEN`) or on another desktop
 * is hidden, so is a server-reported `VisibilityNotify(FullyObscured)`, and a
 * low-rate `_NET_CLIENT_LIST_STACKING` cover check applies the rules above
 * where X has the same facts.
 *
 * Desktop Linux, native Wayland (u_window_visibility_wayland.c, #2689): a
 * Wayland client cannot see the global stacking order, but GNOME Shell can, and
 * DisplayXR's own window-geometry@displayxr.org extension (version 20+)
 * publishes a per-window `hidden` reason (`minimized`, `workspace`, `locked`,
 * `covered`) computed in gnome-shell with the X11 occluder rules. The tracker
 * reads it for this process's window, picked the way the vk_native geometry
 * consumer picks it (@ref u_wv_wl_parse_snapshot). No extension, an older one
 * (no `hidden`), or another desktop: not tracked, the window counts as
 * visible. Created with @ref u_window_visibility_create_wayland.
 *
 * macOS (u_window_visibility_macos.m): `NSWindow.occlusionState`, sampled on
 * the main thread (never waited on), which already folds in minimized, other
 * Space, hidden app and full cover; it is window-wide, not clipped to the 3D
 * screen.
 *
 * Desktop Linux and macOS judge the whole window for every screen (no segment
 * DPs there) and report no foreground fact.
 *
 * Other platforms (Android) get a tracker that never reports hidden.
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

//! What GNOME Shell's window snapshot says about one process's window (#2689).
enum u_wv_wl_state
{
	//! Not a snapshot this reader understands (not JSON, no `windows`, a
	//! schema `version` above 1).
	U_WV_WL_UNKNOWN = 0,
	//! No (sized) window of the process in it: not mapped yet, or gone.
	U_WV_WL_NO_WINDOW,
	//! The window has no `hidden` key: an extension older than version 20.
	U_WV_WL_NO_FIELD,
	//! `hidden` is "": the window can be seen.
	U_WV_WL_VISIBLE,
	//! `hidden` names a reason.
	U_WV_WL_HIDDEN,
};

/*!
 * Read a `GetWindows` / `WindowsChanged` payload of the window-geometry
 * extension (docs/specs/runtime/wayland-window-geometry.md §4.3) for the
 * window of process @p pid. The window is picked the way
 * comp_vk_native_wl_geom_get_window_rect picks it, so both read the same
 * window: owned by @p pid with a non-empty frame, the focused one first, else
 * the largest frame.
 *
 * @p out_reason (optional) gets a static string: for HIDDEN the reason as the
 * transition line words it ("minimized", "on another workspace", "screen
 * locked", "fully covered", or "hidden by the shell" for a reason this
 * reader does not know), else "uncovered".
 */
enum u_wv_wl_state
u_wv_wl_parse_snapshot(const char *json, int32_t pid, const char **out_reason);

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
 * Start tracking @p native_window. Per platform:
 * - Windows: an HWND; the tracker walks from its root window.
 * - desktop Linux: an X11 window XID cast to a pointer
 *   (`(void *)(uintptr_t)xid`) on the server `$DISPLAY` names; it may be a
 *   child, the tracker walks up to its managed top-level.
 * - macOS: an `NSView *` or `NSWindow *` bridged to `void *`.
 *
 * @p panel_device_name names the 3D screen,
 * `xrt_system_compositor_info::display_device_name` (Windows: the GDI device
 * name; desktop Linux: the RandR monitor name; macOS: ignored); NULL or ""
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
 * #2689: start tracking a native Wayland window (desktop Linux; NULL on every
 * other platform). @p wl_surface is the app's `wl_surface *` and only names
 * the target (a different pointer through @ref u_window_visibility_set_window
 * is a new window): the window itself is this process's, found in GNOME
 * Shell's window snapshot as @ref u_wv_wl_parse_snapshot picks it. The
 * tracker opens its own session-bus connection and uses it from its thread
 * only, so it keeps working while a hidden surface's frame loop is throttled.
 *
 * NULL also when the session bus is unreachable or the desktop is not GNOME
 * (one INFO line). An extension that is absent or older than version 20 gives
 * a tracker that reports visible until a snapshot with `hidden` arrives (one
 * INFO line says which). Everything else as @ref u_window_visibility_create.
 */
struct u_window_visibility *
u_window_visibility_create_wayland(void *wl_surface,
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
	/*!
	 * ADR-062 D4 (tier 3): debounced, hidden on every screen the window
	 * covers — @ref hidden_everywhere without the tier-2 switch, so the app
	 * is told the same thing whether or not tier 2 is on.
	 */
	bool hidden_every_screen;
	//! ADR-062 D4: why, judged over every screen, as of the last check.
	enum u_wv_reason reason_every_screen;
};

/*!
 * Fill @p out (zeroed first). NULL-safe: a NULL tracker reports
 * `tracked = false`. Any thread; a short shared lock.
 */
void
u_window_visibility_get_state(const struct u_window_visibility *wv, struct u_window_visibility_state *out);

/*!
 * ADR-062 D4 (tier 3): the tracked window's root is the OS foreground window
 * (Windows: GetForegroundWindow's root window equals the target's). Asked on
 * call, never cached. False for a NULL tracker, no window, or a platform
 * without the fact. Any thread; a short shared lock and two cheap calls.
 */
bool
u_window_visibility_foreground(const struct u_window_visibility *wv);

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
