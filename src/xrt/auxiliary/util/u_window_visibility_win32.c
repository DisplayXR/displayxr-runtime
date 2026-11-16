// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window visibility for the lens wish (#2671, ADR-062 D1): the
 *         Windows tracker.
 * @ingroup aux_util
 *
 * One thread per tracked window. It owns a message-only window (session lock
 * and console display-state notifications) and out-of-context WinEvent hooks
 * (foreground, minimize, show/hide, cloak, move/size), so it runs a message
 * loop: an event schedules a re-check a few ms later, and a slow safety poll
 * covers everything the events miss. Every check runs with the thread pinned
 * to per-monitor-v2 DPI awareness, so every rect is physical whatever the
 * host process declared (docs/reference/dpi-awareness.md: this code runs
 * inside the app's process on the in-process routes).
 */

#include "util/u_window_visibility.h"
#include "util/u_logging.h"
#include "util/u_debug.h"
#include "os/os_time.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif
#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif
#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#endif
#ifndef EVENT_OBJECT_UNCLOAKED
#define EVENT_OBJECT_UNCLOAKED 0x8018
#endif
#ifndef WM_WTSSESSION_CHANGE
#define WM_WTSSESSION_CHANGE 0x02B1
#endif
#ifndef WTS_CONSOLE_CONNECT
#define WTS_CONSOLE_CONNECT 0x1
#endif
#ifndef WTS_CONSOLE_DISCONNECT
#define WTS_CONSOLE_DISCONNECT 0x2
#endif
#ifndef WTS_SESSION_LOCK
#define WTS_SESSION_LOCK 0x7
#endif
#ifndef WTS_SESSION_UNLOCK
#define WTS_SESSION_UNLOCK 0x8
#endif
#ifndef NOTIFY_FOR_THIS_SESSION
#define NOTIFY_FOR_THIS_SESSION 0
#endif
#ifndef ABS_AUTOHIDE
#define ABS_AUTOHIDE 0x1
#endif
#ifndef ABM_GETSTATE
#define ABM_GETSTATE 0x4
#endif

#ifdef _MSC_VER
#define WV_THREAD_LOCAL __declspec(thread)
#else
#define WV_THREAD_LOCAL __thread
#endif

//! GUID_CONSOLE_DISPLAY_STATE {6FE69556-704A-47A0-8F24-C28D936FDA47}: 0 off, 1 on, 2 dimmed.
static const GUID k_console_display_state = {
    0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

//! Most windows walked above the target per check (bounds a pathological desktop).
#define WV_MAX_WALK 1024
//! Most occluders kept per check; a cover needs far fewer.
#define WV_MAX_OCCLUDERS 256
//! Process-name cache size and lifetime (pid reuse).
#define WV_PID_CACHE 32
#define WV_PID_CACHE_TTL_NS (10ull * 1000ull * 1000ull * 1000ull)
//! WinEvent hook ranges (each range its own hook, so nothing in between is delivered).
#define WV_MAX_HOOKS 8

#define WV_MS_NS (1000ull * 1000ull)

//! One extra 3D screen (ADR-047 segment DP). Name and hidden: under panel_lock / atomic.
struct wv_screen
{
	char name[128];
	//! Debounced result for this screen, read by any thread.
	volatile LONG hidden;
};

struct wv_pid_entry
{
	DWORD pid;
	char name[MAX_PATH];
};

typedef BOOL(WINAPI *wv_wts_register_fn)(HWND, DWORD);
typedef BOOL(WINAPI *wv_wts_unregister_fn)(HWND);
typedef UINT_PTR(WINAPI *wv_appbar_fn)(DWORD, PAPPBARDATA);

struct u_window_visibility
{
	HWND target;
	char who[64];
	u_window_visibility_change_fn on_change;
	void *userdata;
	char extra_overlays[512];

	//! Guards @ref panel, @ref next_target, @ref retarget, @ref screens and the published state.
	SRWLOCK panel_lock;
	char panel[128];
	HWND next_target;
	bool retarget;
	//! Extra 3D screens (u_window_visibility_set_screens); @ref screens_gen bumps on change.
	struct wv_screen screens[U_WINDOW_VISIBILITY_MAX_SCREENS];
	uint32_t screen_count;
	uint32_t screens_gen;
	//! Published for u_window_visibility_get_state (under panel_lock).
	enum u_wv_reason pub_reason;
	char pub_occluder[U_WINDOW_VISIBILITY_NAME_MAX];
	uint64_t pub_since_ns;

	//! Debounced results, read by any thread.
	volatile LONG hidden;
	volatile LONG hidden_all;

	HANDLE stop_event;
	//! Auto-reset: a setter changed something, check now.
	HANDLE wake_event;
	HANDLE thread;

	// Thread-private state below.
	struct u_wv_debounce debounce;
	struct u_wv_debounce debounce_all;
	DWORD target_pid;
	bool ever_shown;
	bool locked;
	bool display_off;
	bool movesize_active;
	uint64_t movesize_since_ns;
	//! An event asked for a check no later than this (0 = none).
	uint64_t kick_ns;
	enum u_wv_reason raw_reason;
	enum u_wv_reason raw_reason_all;
	char occluder[U_WINDOW_VISIBILITY_NAME_MAX];
	int taskbar_auto_hide; //!< -1 = not queried this check
	wv_appbar_fn appbar;
	struct wv_pid_entry pids[WV_PID_CACHE];
	uint32_t pid_count;
	uint64_t pid_cache_ns;
	struct u_wv_rect occ[WV_MAX_OCCLUDERS];
	//! Per extra screen: debounce, last raw verdict, and the list generation they belong to.
	struct u_wv_debounce screen_debounce[U_WINDOW_VISIBILITY_MAX_SCREENS];
	enum u_wv_part screen_raw[U_WINDOW_VISIBILITY_MAX_SCREENS];
	uint32_t screens_gen_seen;

	/*
	 * ADR-062 D5 (tier 4): the visible region. Published under panel_lock
	 * (pub_region, pub_screen_*); its generation is mirrored in region_gen for
	 * a lock-free "did anything move" read.
	 */
	struct u_window_visibility_region pub_region;
	//! Client-relative rects of the session's 3D screen (slot 0) and the extra screens.
	struct u_wv_rect pub_screen_rect[1 + U_WINDOW_VISIBILITY_MAX_SCREENS];
	bool pub_screen_found[1 + U_WINDOW_VISIBILITY_MAX_SCREENS];
	volatile LONG64 region_gen;
	// Thread-private: the last raw region and its debounce.
	bool raw_region_known;
	struct u_wv_region raw_region;
	struct u_wv_rect raw_region_whole; //!< client-relative, the merge fallback
	uint32_t raw_client_w;
	uint32_t raw_client_h;
	struct u_wv_rect raw_screen_rect[1 + U_WINDOW_VISIBILITY_MAX_SCREENS];
	bool raw_screen_found[1 + U_WINDOW_VISIBILITY_MAX_SCREENS];
	struct u_wv_region_debounce region_debounce;
	uint64_t region_gen_next;
};

// Resolved dynamically, as os_display_desktop_win32.c does: Windows 10 1607+,
// and MinGW headers may not declare it. HANDLE stands in for DPI_AWARENESS_CONTEXT.
typedef HANDLE wv_dpi_context_t;
typedef wv_dpi_context_t(WINAPI *wv_set_thread_dpi_fn)(wv_dpi_context_t);
#define WV_DPI_PER_MONITOR_AWARE_V2 ((wv_dpi_context_t)(intptr_t)-4)

//! The tracker served by this thread (the hook and window callbacks carry no user data).
static WV_THREAD_LOCAL struct u_window_visibility *t_wv = NULL;

/*
 *
 * Helpers.
 *
 */

static struct u_wv_rect
wv_from_rect(const RECT *r)
{
	struct u_wv_rect o = {r->left, r->top, r->right, r->bottom};
	return o;
}

static bool
wv_cloaked(HWND w)
{
	DWORD cloaked = 0;
	return SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

static bool
wv_frame_bounds(HWND w, struct u_wv_rect *out)
{
	RECT r;
	if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
		if (!GetWindowRect(w, &r)) {
			return false;
		}
	}
	*out = wv_from_rect(&r);
	return true;
}

static uint64_t
wv_area(const struct u_wv_rect *r)
{
	return u_wv_rect_empty(r) ? 0 : (uint64_t)(r->right - r->left) * (uint64_t)(r->bottom - r->top);
}

//! Ask for a check no later than @p delay_ms from now.
static void
wv_kick(struct u_window_visibility *wv, uint32_t delay_ms)
{
	const uint64_t t = os_monotonic_get_ns() + (uint64_t)delay_ms * WV_MS_NS;
	if (wv->kick_ns == 0 || t < wv->kick_ns) {
		wv->kick_ns = t;
	}
}

//! Slot 0: the session's 3D screen; 1..: the extra screens.
#define WV_MAX_NAMES (1 + U_WINDOW_VISIBILITY_MAX_SCREENS)

struct wv_monitor_find
{
	char name[WV_MAX_NAMES][128];
	RECT rect[WV_MAX_NAMES];
	bool found[WV_MAX_NAMES];
	uint32_t count;
};

static BOOL CALLBACK
wv_monitor_cb(HMONITOR mon, HDC dc, LPRECT rc, LPARAM lp)
{
	(void)dc;
	(void)rc;
	struct wv_monitor_find *f = (struct wv_monitor_find *)lp;
	MONITORINFOEXA mi;
	memset(&mi, 0, sizeof(mi));
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoA(mon, (LPMONITORINFO)&mi)) {
		return TRUE;
	}
	for (uint32_t i = 0; i < f->count; i++) {
		if (!f->found[i] && f->name[i][0] != '\0' && _stricmp(mi.szDevice, f->name[i]) == 0) {
			f->rect[i] = mi.rcMonitor;
			f->found[i] = true;
		}
	}
	return TRUE;
}

/*!
 * Resolve the session's 3D screen (slot 0) and every extra screen (slots 1..)
 * to physical rects, by GDI device name, in one monitor enumeration. A changed
 * extra-screen list resets the per-screen dwell here.
 */
static void
wv_resolve_screens(struct u_window_visibility *wv, struct wv_monitor_find *f)
{
	memset(f, 0, sizeof(*f));
	AcquireSRWLockShared(&wv->panel_lock);
	memcpy(f->name[0], wv->panel, sizeof(f->name[0]));
	const uint32_t n = wv->screen_count;
	for (uint32_t i = 0; i < n; i++) {
		memcpy(f->name[1 + i], wv->screens[i].name, sizeof(f->name[0]));
	}
	const uint32_t gen = wv->screens_gen;
	ReleaseSRWLockShared(&wv->panel_lock);
	f->count = 1 + n;
	if (gen != wv->screens_gen_seen) {
		wv->screens_gen_seen = gen;
		memset(wv->screen_debounce, 0, sizeof(wv->screen_debounce));
		for (uint32_t i = 0; i < U_WINDOW_VISIBILITY_MAX_SCREENS; i++) {
			wv->screen_raw[i] = U_WV_PART_OFF;
		}
	}
	EnumDisplayMonitors(NULL, NULL, wv_monitor_cb, (LPARAM)f);
}

static const char *
wv_process_name(struct u_window_visibility *wv, DWORD pid)
{
	const uint64_t now = os_monotonic_get_ns();
	if (now - wv->pid_cache_ns > WV_PID_CACHE_TTL_NS) {
		wv->pid_count = 0;
		wv->pid_cache_ns = now;
	}
	for (uint32_t i = 0; i < wv->pid_count; i++) {
		if (wv->pids[i].pid == pid) {
			return wv->pids[i].name;
		}
	}
	struct wv_pid_entry e;
	e.pid = pid;
	e.name[0] = '\0';
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (h != NULL) {
		WCHAR path[MAX_PATH];
		DWORD len = MAX_PATH;
		if (QueryFullProcessImageNameW(h, 0, path, &len)) {
			const WCHAR *base = path;
			for (const WCHAR *p = path; *p != 0; p++) {
				if (*p == L'\\' || *p == L'/') {
					base = p + 1;
				}
			}
			WideCharToMultiByte(CP_UTF8, 0, base, -1, e.name, (int)sizeof(e.name), NULL, NULL);
		}
		CloseHandle(h);
	}
	if (wv->pid_count < WV_PID_CACHE) {
		wv->pids[wv->pid_count] = e;
		return wv->pids[wv->pid_count++].name;
	}
	// Cache full: keep the answer for this call only.
	wv->pids[WV_PID_CACHE - 1] = e;
	return wv->pids[WV_PID_CACHE - 1].name;
}

static bool
wv_layered_not_opaque(HWND w, LONG_PTR ex)
{
	if ((ex & WS_EX_LAYERED) == 0) {
		return false;
	}
	COLORREF key = 0;
	BYTE alpha = 255;
	DWORD flags = 0;
	// Fails for per-pixel-alpha (UpdateLayeredWindow) windows: not opaque.
	if (!GetLayeredWindowAttributes(w, &key, &alpha, &flags)) {
		return true;
	}
	if ((flags & LWA_COLORKEY) != 0) {
		return true;
	}
	return (flags & LWA_ALPHA) != 0 && alpha < 255;
}

//! The taskbar's auto-hide setting, asked at most once per check.
static bool
wv_taskbar_auto_hide(struct u_window_visibility *wv)
{
	if (wv->taskbar_auto_hide < 0) {
		wv->taskbar_auto_hide = 0;
		if (wv->appbar != NULL) {
			APPBARDATA abd;
			memset(&abd, 0, sizeof(abd));
			abd.cbSize = sizeof(abd);
			wv->taskbar_auto_hide = (wv->appbar(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) != 0 ? 1 : 0;
		}
	}
	return wv->taskbar_auto_hide != 0;
}

/*
 *
 * The check.
 *
 */

//! Set the first @p n extra screens' raw verdict at once.
static void
wv_screens_raw(struct u_window_visibility *wv, uint32_t n, enum u_wv_part state)
{
	for (uint32_t i = 0; i < n && i < U_WINDOW_VISIBILITY_MAX_SCREENS; i++) {
		wv->screen_raw[i] = state;
	}
}

/*!
 * One raw check. Sets wv->raw_reason (the session's 3D screen),
 * wv->raw_reason_all (every screen, 2D ones included), wv->screen_raw[] (the
 * extra screens) and wv->occluder (the largest occluder's process).
 */
static void
wv_check(struct u_window_visibility *wv)
{
	struct wv_monitor_find f;
	wv_resolve_screens(wv, &f);
	const uint32_t extra = f.count - 1;
	wv->occluder[0] = '\0';
	wv->taskbar_auto_hide = -1;
	wv->raw_region_known = false;
	memset(&wv->raw_region, 0, sizeof(wv->raw_region));

	HWND target = wv->target;
	struct u_wv_window_state ws;
	memset(&ws, 0, sizeof(ws));
	ws.exists = IsWindow(target) != FALSE;
	ws.locked = wv->locked;
	ws.display_off = wv->display_off;
	HWND root = NULL;
	if (ws.exists) {
		root = GetAncestor(target, GA_ROOT);
		if (root == NULL) {
			root = target;
		}
		ws.visible = IsWindowVisible(root) && IsWindowVisible(target);
		// An OWNED top-level window (an engine's overlay popup owned by its
		// main window) is hidden, not iconic, when its owner is minimized:
		// report that as minimized, not as "not visible", so the minimize
		// rules (ADR-062 D1/D4) apply to it the same as to the owner.
		HWND root_owner = GetAncestor(target, GA_ROOTOWNER);
		ws.minimized = IsIconic(root) != FALSE || (root_owner != NULL && root_owner != root && IsIconic(root_owner));
		ws.cloaked = wv_cloaked(root);
		if (ws.visible) {
			wv->ever_shown = true;
		}
		ws.ever_shown = wv->ever_shown;
	}
	const enum u_wv_reason wr = u_wv_window_reason(&ws);
	if (wr != U_WV_REASON_VISIBLE) {
		// Hidden reasons hide the window on every screen; not-shown and gone
		// on none (its rect means nothing then).
		wv->raw_reason = wr;
		wv->raw_reason_all = wr;
		wv_screens_raw(wv, extra, u_wv_reason_is_hidden(wr) ? U_WV_PART_HIDDEN : U_WV_PART_OFF);
		// Tier 4: a hidden window shows nothing (known, empty); one not shown
		// yet or gone is not known (the whole window: the not-hidden side).
		wv->raw_region_known = u_wv_reason_is_hidden(wr);
		return;
	}

	RECT cr;
	POINT tl = {0, 0};
	POINT br = {0, 0};
	if (GetClientRect(target, &cr)) {
		tl.x = cr.left;
		tl.y = cr.top;
		br.x = cr.right;
		br.y = cr.bottom;
		ClientToScreen(target, &tl);
		ClientToScreen(target, &br);
	}
	// Occluders are gathered against the whole client rect, so one z-walk
	// serves the session's 3D screen, every extra one and the whole desktop.
	const struct u_wv_rect client = {tl.x, tl.y, br.x, br.y};
	if (u_wv_rect_empty(&client)) {
		wv->raw_reason = U_WV_REASON_OFF_SCREEN;
		wv->raw_reason_all = U_WV_REASON_OFF_SCREEN;
		wv_screens_raw(wv, extra, U_WV_PART_OFF);
		return;
	}
	struct u_wv_rect t = client;
	if (f.found[0]) {
		const struct u_wv_rect panel = wv_from_rect(&f.rect[0]);
		t = u_wv_rect_intersect(&t, &panel);
	}
	// Every screen: the client rect clipped to the virtual desktop.
	const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
	const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
	const struct u_wv_rect desktop = {vx, vy, vx + GetSystemMetrics(SM_CXVIRTUALSCREEN),
	                                  vy + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
	const struct u_wv_rect all = u_wv_rect_intersect(&client, &desktop);

	if (wv->target_pid == 0) {
		GetWindowThreadProcessId(root, &wv->target_pid);
	}
	const DWORD self_pid = GetCurrentProcessId();

	size_t n = 0;
	uint64_t largest = 0;
	int walked = 0;
	for (HWND w = GetWindow(root, GW_HWNDPREV); w != NULL && walked < WV_MAX_WALK;
	     w = GetWindow(w, GW_HWNDPREV), walked++) {
		if (!IsWindowVisible(w) || IsIconic(w)) {
			continue;
		}
		// Cheap pre-filter: the plain window rect (it contains the frame bounds).
		RECT wr_rect;
		if (!GetWindowRect(w, &wr_rect)) {
			continue;
		}
		const struct u_wv_rect wrr = wv_from_rect(&wr_rect);
		const struct u_wv_rect pre = u_wv_rect_intersect(&wrr, &client);
		if (u_wv_rect_empty(&pre)) {
			continue;
		}
		struct u_wv_rect fb;
		if (!wv_frame_bounds(w, &fb)) {
			continue;
		}
		const struct u_wv_rect x = u_wv_rect_intersect(&fb, &client);
		if (u_wv_rect_empty(&x)) {
			continue;
		}

		DWORD pid = 0;
		GetWindowThreadProcessId(w, &pid);
		char cls[128];
		cls[0] = '\0';
		GetClassNameA(w, cls, (int)sizeof(cls));
		const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);

		struct u_wv_window_facts fa;
		memset(&fa, 0, sizeof(fa));
		fa.class_name = cls;
		fa.visible = true;
		fa.same_process = pid == wv->target_pid || pid == self_pid;
		fa.cloaked = wv_cloaked(w);
		fa.no_redirection_bitmap =
		    (ex & WS_EX_NOREDIRECTIONBITMAP) != 0 && strcmp(cls, "ApplicationFrameWindow") == 0;
		fa.input_transparent = (ex & WS_EX_TRANSPARENT) != 0;
		fa.layered_not_opaque = wv_layered_not_opaque(w, ex);
		if (strcmp(cls, "Shell_TrayWnd") == 0 || strcmp(cls, "Shell_SecondaryTrayWnd") == 0) {
			fa.taskbar_auto_hide = wv_taskbar_auto_hide(wv);
		}
		// Only open the process when nothing cheaper already decided.
		fa.process_name = "";
		if (u_wv_classify_occluder(&fa, NULL) == U_WV_OCCLUDES) {
			fa.process_name = wv_process_name(wv, pid);
		}
		if (u_wv_classify_occluder(&fa, wv->extra_overlays) != U_WV_OCCLUDES) {
			continue;
		}
		if (n < WV_MAX_OCCLUDERS) {
			wv->occ[n++] = x;
		}
		const uint64_t a = wv_area(&x);
		if (a > largest) {
			largest = a;
			(void)snprintf(wv->occluder, sizeof(wv->occluder), "%s",
			               fa.process_name[0] != '\0' ? fa.process_name : cls);
		}
	}

	for (uint32_t i = 0; i < extra; i++) {
		if (!f.found[1 + i]) {
			wv->screen_raw[i] = U_WV_PART_OFF; // unplugged or unnamed: never hidden
			continue;
		}
		const struct u_wv_rect sr = wv_from_rect(&f.rect[1 + i]);
		wv->screen_raw[i] = u_wv_part_state(&client, &sr, wv->occ, n);
	}

	const enum u_wv_reason vis = n > 0 ? U_WV_REASON_PARTLY_COVERED : U_WV_REASON_VISIBLE;
	wv->raw_reason_all = u_wv_rect_mostly_covered(&all, wv->occ, n) ? U_WV_REASON_COVERED : vis;

	// Tier 4: the uncovered remainder, window-client relative, from the same
	// z-walk. Fragmenting past the cap falls back to the whole part.
	(void)u_wv_region_compute(&all, wv->occ, n, &wv->raw_region);
	u_wv_region_translate(&wv->raw_region, -tl.x, -tl.y);
	wv->raw_region_whole = all;
	wv->raw_region_whole.left -= tl.x;
	wv->raw_region_whole.right -= tl.x;
	wv->raw_region_whole.top -= tl.y;
	wv->raw_region_whole.bottom -= tl.y;
	wv->raw_client_w = (uint32_t)(br.x - tl.x);
	wv->raw_client_h = (uint32_t)(br.y - tl.y);
	for (uint32_t i = 0; i < 1 + U_WINDOW_VISIBILITY_MAX_SCREENS; i++) {
		wv->raw_screen_found[i] = i < f.count && f.found[i];
		if (wv->raw_screen_found[i]) {
			struct u_wv_rect sr = wv_from_rect(&f.rect[i]);
			sr.left -= tl.x;
			sr.right -= tl.x;
			sr.top -= tl.y;
			sr.bottom -= tl.y;
			wv->raw_screen_rect[i] = sr;
		}
	}
	wv->raw_region_known = true;
	if (u_wv_rect_empty(&t)) {
		// Nothing on the 3D screen: the multi-screen coverage gate owns that case.
		wv->raw_reason = U_WV_REASON_OFF_SCREEN;
		return;
	}
	wv->raw_reason = u_wv_rect_mostly_covered(&t, wv->occ, n) ? U_WV_REASON_COVERED : vis;
}

//! Debounce every extra screen's raw verdict; one WARN per screen edge.
static void
wv_update_screens(struct u_window_visibility *wv, uint64_t now_ns, bool immediate, bool hold)
{
	char names[U_WINDOW_VISIBILITY_MAX_SCREENS][128];
	char panel[sizeof(wv->panel)];
	AcquireSRWLockShared(&wv->panel_lock);
	memcpy(panel, wv->panel, sizeof(panel));
	const uint32_t n = wv->screen_count;
	const bool same_gen = wv->screens_gen == wv->screens_gen_seen;
	for (uint32_t i = 0; i < n; i++) {
		memcpy(names[i], wv->screens[i].name, sizeof(names[i]));
	}
	ReleaseSRWLockShared(&wv->panel_lock);
	if (!same_gen) {
		return; // the list changed under this check; the next one starts fresh
	}
	for (uint32_t i = 0; i < n; i++) {
		const bool raw = wv->screen_raw[i] == U_WV_PART_HIDDEN;
		const bool was = wv->screen_debounce[i].reported_hidden;
		const bool now = u_wv_debounce_step(&wv->screen_debounce[i], raw, immediate, hold, now_ns,
		                                    U_WINDOW_VISIBILITY_HIDE_DWELL_NS);
		if (now != was) {
			InterlockedExchange(&wv->screens[i].hidden, now ? 1 : 0);
			// The session's own screen already logs through the main transition.
			if (_stricmp(names[i], panel) == 0) {
				continue;
			}
			U_LOG_W("#2671 %s: the window's part on 3D screen '%s' is %s - that screen's lens wish %s",
			        wv->who, names[i], now ? "hidden" : "visible again",
			        now ? "2D" : "back to the session's mode");
		}
	}
}

//! The earliest pending dwell deadline of every debounce (0 = none).
static uint64_t
wv_next_deadline(const struct u_window_visibility *wv)
{
	uint64_t d = 0;
	const uint64_t c[2] = {u_wv_debounce_deadline(&wv->debounce, U_WINDOW_VISIBILITY_HIDE_DWELL_NS),
	                       u_wv_debounce_deadline(&wv->debounce_all, U_WINDOW_VISIBILITY_HIDE_DWELL_NS)};
	for (int i = 0; i < 2; i++) {
		if (c[i] != 0 && (d == 0 || c[i] < d)) {
			d = c[i];
		}
	}
	for (uint32_t i = 0; i < U_WINDOW_VISIBILITY_MAX_SCREENS; i++) {
		const uint64_t s = u_wv_debounce_deadline(&wv->screen_debounce[i], U_WINDOW_VISIBILITY_HIDE_DWELL_NS);
		if (s != 0 && (d == 0 || s < d)) {
			d = s;
		}
	}
	const uint64_t r = u_wv_region_debounce_deadline(&wv->region_debounce, U_WINDOW_VISIBILITY_HIDE_DWELL_NS);
	if (r != 0 && (d == 0 || r < d)) {
		d = r;
	}
	return d;
}

static void
wv_publish(struct u_window_visibility *wv, bool edge, uint64_t now_ns)
{
	AcquireSRWLockExclusive(&wv->panel_lock);
	wv->pub_reason = wv->raw_reason;
	memcpy(wv->pub_occluder, wv->occluder, sizeof(wv->pub_occluder));
	if (edge) {
		wv->pub_since_ns = now_ns;
	}
	ReleaseSRWLockExclusive(&wv->panel_lock);
}

/*!
 * Tier 4: debounce the raw region and publish it when anything changed (the
 * region, its known flag, the client size or a screen rect). Tracker thread.
 */
static void
wv_update_region(struct u_window_visibility *wv, uint64_t now_ns, bool immediate, bool hold)
{
	const struct u_window_visibility_region *pub = &wv->pub_region;
	bool changed = pub->gen == 0 || pub->known != wv->raw_region_known;
	if (!wv->raw_region_known) {
		wv->region_debounce.init = false;
	} else {
		if (pub->client_w != wv->raw_client_w || pub->client_h != wv->raw_client_h) {
			// A resize re-bases every rect: start over at the new size.
			wv->region_debounce.init = false;
			changed = true;
		}
		const struct u_wv_region *raw = &wv->raw_region;
		if (u_wv_region_debounce_step(&wv->region_debounce, raw, immediate, hold, now_ns,
		                              U_WINDOW_VISIBILITY_HIDE_DWELL_NS, &wv->raw_region_whole)) {
			changed = true;
		}
		for (uint32_t i = 0; !changed && i < 1 + U_WINDOW_VISIBILITY_MAX_SCREENS; i++) {
			changed = wv->raw_screen_found[i] != wv->pub_screen_found[i] ||
			          (wv->raw_screen_found[i] &&
			           memcmp(&wv->raw_screen_rect[i], &wv->pub_screen_rect[i], sizeof(struct u_wv_rect)) != 0);
		}
	}
	if (!changed) {
		return;
	}
	AcquireSRWLockExclusive(&wv->panel_lock);
	struct u_window_visibility_region *p = &wv->pub_region;
	p->gen = ++wv->region_gen_next;
	p->known = wv->raw_region_known;
	if (p->known) {
		p->client_w = wv->raw_client_w;
		p->client_h = wv->raw_client_h;
		p->region = wv->region_debounce.pub;
		memcpy(wv->pub_screen_rect, wv->raw_screen_rect, sizeof(wv->pub_screen_rect));
		memcpy(wv->pub_screen_found, wv->raw_screen_found, sizeof(wv->pub_screen_found));
	} else {
		memset(&p->region, 0, sizeof(p->region));
	}
	const uint64_t gen = p->gen;
	const uint32_t count = p->region.count;
	const bool whole = p->region.whole;
	const uint64_t area = u_wv_region_area(&p->region);
	const uint64_t full = (uint64_t)p->client_w * (uint64_t)p->client_h;
	ReleaseSRWLockExclusive(&wv->panel_lock);
	InterlockedExchange64(&wv->region_gen, (LONG64)gen);
	// INFO, not WARN: a region moves with every window drag.
	U_LOG_I("ADR-062 %s: visible region gen %llu: %s, %u rect(s)%s, %llu of %llu px", wv->who,
	        (unsigned long long)gen, wv->raw_region_known ? "known" : "unknown (whole window)", count,
	        whole ? " (fragmented: whole)" : "", (unsigned long long)area, (unsigned long long)full);
}

//! Readers see "nothing known" from now on (a new window, or the tracker stopped).
static void
wv_region_forget(struct u_window_visibility *wv)
{
	AcquireSRWLockExclusive(&wv->panel_lock);
	wv->pub_region.gen = ++wv->region_gen_next;
	wv->pub_region.known = false;
	memset(&wv->pub_region.region, 0, sizeof(wv->pub_region.region));
	const uint64_t gen = wv->pub_region.gen;
	ReleaseSRWLockExclusive(&wv->panel_lock);
	InterlockedExchange64(&wv->region_gen, (LONG64)gen);
}

//! One full check + debounce + transitions. Tracker thread.
static void
wv_tick(struct u_window_visibility *wv)
{
	wv_check(wv);
	const uint64_t now_ns = os_monotonic_get_ns();
	if (wv->movesize_active && now_ns - wv->movesize_since_ns > U_WINDOW_VISIBILITY_MOVESIZE_TIMEOUT_NS) {
		wv->movesize_active = false; // an end event we never saw
	}
	const bool hold = wv->movesize_active;
	const bool immediate = u_wv_reason_is_immediate(wv->raw_reason);
	wv_update_screens(wv, now_ns, immediate, hold);
	wv_update_region(wv, now_ns, u_wv_reason_is_immediate(wv->raw_reason_all), hold);

	const bool was = wv->debounce.reported_hidden;
	const bool now = u_wv_debounce_step(&wv->debounce, u_wv_reason_is_hidden(wv->raw_reason), immediate, hold,
	                                    now_ns, U_WINDOW_VISIBILITY_HIDE_DWELL_NS);
	const bool was_all = wv->debounce_all.reported_hidden;
	const bool now_all =
	    u_wv_debounce_step(&wv->debounce_all, u_wv_reason_is_hidden(wv->raw_reason_all),
	                       u_wv_reason_is_immediate(wv->raw_reason_all), hold, now_ns, U_WINDOW_VISIBILITY_HIDE_DWELL_NS);

	wv_publish(wv, now != was || now_all != was_all, now_ns);

	// Edge ordering (ADR-062 D3): the lens edge (the callback) always runs
	// before the tier-2 flag moves, so a hidden window's lens is 2D before
	// its weave stops, and the session's lens request goes out before the
	// first weave after it is seen again.
	if (now != was) {
		InterlockedExchange(&wv->hidden, now ? 1 : 0);
		const bool covered = wv->raw_reason == U_WV_REASON_COVERED;
		U_LOG_W("#2671 %s: the 3D window is %s (%s%s%s) - lens wish %s", wv->who,
		        now ? "hidden" : "visible again", u_wv_reason_str(wv->raw_reason),
		        covered && wv->occluder[0] != '\0' ? ", largest occluder " : "",
		        covered ? wv->occluder : "",
		        now ? "2D unless another 3D window is visible" : "back to the session's mode");
		if (wv->on_change != NULL) {
			wv->on_change(wv->userdata, now);
		}
	}
	if (now_all != was_all) {
		InterlockedExchange(&wv->hidden_all, now_all ? 1 : 0);
		if (u_window_visibility_skip_present_enabled()) {
			U_LOG_W("ADR-062 %s: the window is %s on every screen (%s) - weave and present %s", wv->who,
			        now_all ? "hidden" : "visible", u_wv_reason_str(wv->raw_reason_all),
			        now_all ? "skipped while hidden" : "resume (the retained frame is re-woven first)");
		}
	}
}

/*
 *
 * Events.
 *
 */

static void CALLBACK
wv_win_event(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG id_object, LONG id_child, DWORD thread, DWORD time)
{
	(void)hook;
	(void)thread;
	(void)time;
	struct u_window_visibility *wv = t_wv;
	if (wv == NULL) {
		return;
	}
	switch (event) {
	case EVENT_SYSTEM_MOVESIZESTART:
		wv->movesize_active = true;
		wv->movesize_since_ns = os_monotonic_get_ns();
		return;
	case EVENT_SYSTEM_MOVESIZEEND:
		wv->movesize_active = false;
		wv_kick(wv, U_WINDOW_VISIBILITY_EVENT_DELAY_MS);
		return;
	case EVENT_SYSTEM_FOREGROUND:
	case EVENT_SYSTEM_MINIMIZESTART:
	case EVENT_SYSTEM_MINIMIZEEND: wv_kick(wv, U_WINDOW_VISIBILITY_EVENT_DELAY_MS); return;
	default: break;
	}
	// Object events: top-level windows only (the cursor and caret raise
	// location changes too, many per second).
	if (id_object != OBJID_WINDOW || id_child != CHILDID_SELF || hwnd == NULL ||
	    GetAncestor(hwnd, GA_ROOT) != hwnd) {
		return;
	}
	wv_kick(wv, event == EVENT_OBJECT_LOCATIONCHANGE ? U_WINDOW_VISIBILITY_MOVE_DELAY_MS
	                                                 : U_WINDOW_VISIBILITY_EVENT_DELAY_MS);
}

static LRESULT CALLBACK
wv_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	struct u_window_visibility *wv = t_wv;
	if (wv != NULL && msg == WM_WTSSESSION_CHANGE) {
		if (wp == WTS_SESSION_LOCK || wp == WTS_CONSOLE_DISCONNECT) {
			wv->locked = true;
		} else if (wp == WTS_SESSION_UNLOCK || wp == WTS_CONSOLE_CONNECT) {
			wv->locked = false;
		}
		wv_kick(wv, 0);
		return 0;
	}
	if (wv != NULL && msg == WM_POWERBROADCAST && wp == PBT_POWERSETTINGCHANGE && lp != 0) {
		const POWERBROADCAST_SETTING *ps = (const POWERBROADCAST_SETTING *)lp;
		if (memcmp(&ps->PowerSetting, &k_console_display_state, sizeof(GUID)) == 0 &&
		    ps->DataLength >= sizeof(DWORD)) {
			DWORD v = 0;
			memcpy(&v, ps->Data, sizeof(v));
			wv->display_off = v == 0; // 2 = dimmed: still visible
			wv_kick(wv, 0);
		}
		return TRUE;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

struct wv_thread_res
{
	HMODULE self;
	WCHAR class_name[64];
	ATOM atom;
	HWND msg_window;
	HMODULE wtsapi;
	wv_wts_unregister_fn wts_unregister;
	bool wts_registered;
	HPOWERNOTIFY power;
	HWINEVENTHOOK hooks[WV_MAX_HOOKS];
	uint32_t hook_count;
	HMODULE shell32;
};

static void
wv_events_start(struct u_window_visibility *wv, struct wv_thread_res *r)
{
	memset(r, 0, sizeof(*r));
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   (LPCWSTR)(void *)wv_wndproc, &r->self);

	// A class per tracker, unregistered at thread exit: a class a DLL leaves
	// registered outlives the DLL, and a reload would reuse its stale wndproc.
	(void)_snwprintf(r->class_name, sizeof(r->class_name) / sizeof(r->class_name[0]), L"DisplayXRWinVis%p",
	                 (void *)wv);
	WNDCLASSEXW wc;
	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = wv_wndproc;
	wc.hInstance = r->self;
	wc.lpszClassName = r->class_name;
	r->atom = RegisterClassExW(&wc);
	if (r->atom != 0) {
		r->msg_window = CreateWindowExW(0, r->class_name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, r->self, NULL);
	}
	if (r->msg_window != NULL) {
		r->wtsapi = LoadLibraryW(L"wtsapi32.dll");
		if (r->wtsapi != NULL) {
			wv_wts_register_fn reg =
			    (wv_wts_register_fn)(void *)GetProcAddress(r->wtsapi, "WTSRegisterSessionNotification");
			r->wts_unregister =
			    (wv_wts_unregister_fn)(void *)GetProcAddress(r->wtsapi, "WTSUnRegisterSessionNotification");
			r->wts_registered = reg != NULL && r->wts_unregister != NULL &&
			                    reg(r->msg_window, NOTIFY_FOR_THIS_SESSION) != FALSE;
		}
		// Sends the current state at once, then every change.
		r->power = RegisterPowerSettingNotification(r->msg_window, &k_console_display_state,
		                                            DEVICE_NOTIFY_WINDOW_HANDLE);
	}

	static const DWORD ranges[][2] = {
	    {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
	    {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND},
	    {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
	    {EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE},
	    {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
	    {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
	};
	for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]) && r->hook_count < WV_MAX_HOOKS; i++) {
		HWINEVENTHOOK h =
		    SetWinEventHook(ranges[i][0], ranges[i][1], NULL, wv_win_event, 0, 0, WINEVENT_OUTOFCONTEXT);
		if (h != NULL) {
			r->hooks[r->hook_count++] = h;
		}
	}

	r->shell32 = LoadLibraryW(L"shell32.dll");
	if (r->shell32 != NULL) {
		wv->appbar = (wv_appbar_fn)(void *)GetProcAddress(r->shell32, "SHAppBarMessage");
	}
	U_LOG_I("#2671 %s: visibility events: %u hook(s), session notifications %s, display state %s", wv->who,
	        r->hook_count, r->wts_registered ? "on" : "off", r->power != NULL ? "on" : "off");
}

static void
wv_events_stop(struct u_window_visibility *wv, struct wv_thread_res *r)
{
	for (uint32_t i = 0; i < r->hook_count; i++) {
		UnhookWinEvent(r->hooks[i]);
	}
	if (r->power != NULL) {
		UnregisterPowerSettingNotification(r->power);
	}
	if (r->wts_registered) {
		r->wts_unregister(r->msg_window);
	}
	if (r->msg_window != NULL) {
		DestroyWindow(r->msg_window);
	}
	if (r->atom != 0) {
		UnregisterClassW(r->class_name, r->self);
	}
	if (r->wtsapi != NULL) {
		FreeLibrary(r->wtsapi);
	}
	wv->appbar = NULL;
	if (r->shell32 != NULL) {
		FreeLibrary(r->shell32);
	}
}

//! Apply a pending set_window. A new window starts visible: nothing is known about it yet.
static void
wv_apply_retarget(struct u_window_visibility *wv)
{
	AcquireSRWLockExclusive(&wv->panel_lock);
	const bool retarget = wv->retarget;
	if (retarget) {
		wv->retarget = false;
		wv->target = wv->next_target;
	}
	ReleaseSRWLockExclusive(&wv->panel_lock);
	if (!retarget) {
		return;
	}
	wv->target_pid = 0;
	wv->ever_shown = false;
	memset(&wv->debounce, 0, sizeof(wv->debounce));
	memset(&wv->debounce_all, 0, sizeof(wv->debounce_all));
	memset(wv->screen_debounce, 0, sizeof(wv->screen_debounce));
	memset(&wv->region_debounce, 0, sizeof(wv->region_debounce));
	wv_region_forget(wv);
	wv_screens_raw(wv, U_WINDOW_VISIBILITY_MAX_SCREENS, U_WV_PART_OFF);
	for (uint32_t i = 0; i < U_WINDOW_VISIBILITY_MAX_SCREENS; i++) {
		InterlockedExchange(&wv->screens[i].hidden, 0);
	}
	InterlockedExchange(&wv->hidden_all, 0);
	if (InterlockedExchange(&wv->hidden, 0) != 0) {
		U_LOG_W("#2671 %s: tracking a new 3D window - lens wish back to the session's mode", wv->who);
		if (wv->on_change != NULL) {
			wv->on_change(wv->userdata, false);
		}
	}
}

static DWORD WINAPI
wv_thread(LPVOID ptr)
{
	struct u_window_visibility *wv = (struct u_window_visibility *)ptr;
	t_wv = wv;

	wv_dpi_context_t prev = NULL;
	wv_set_thread_dpi_fn set_dpi = NULL;
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	if (user32 != NULL) {
		set_dpi = (wv_set_thread_dpi_fn)(void *)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
	}
	if (set_dpi != NULL) {
		prev = set_dpi(WV_DPI_PER_MONITOR_AWARE_V2);
	}

	struct wv_thread_res res;
	wv_events_start(wv, &res);

	const HANDLE handles[2] = {wv->stop_event, wv->wake_event};
	uint64_t next_ns = os_monotonic_get_ns(); // first check at once
	bool quit = false;
	while (!quit) {
		uint64_t now = os_monotonic_get_ns();
		DWORD timeout = next_ns > now ? (DWORD)((next_ns - now + WV_MS_NS - 1) / WV_MS_NS) : 0;
		const DWORD r = MsgWaitForMultipleObjectsEx(2, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		if (r == WAIT_OBJECT_0) {
			break;
		}
		if (r == WAIT_OBJECT_0 + 1) {
			next_ns = 0; // a setter changed something: check now
		}
		MSG msg;
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				quit = true;
				break;
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (wv->kick_ns != 0 && wv->kick_ns < next_ns) {
			next_ns = wv->kick_ns;
		}
		now = os_monotonic_get_ns();
		if (quit || now < next_ns) {
			continue;
		}
		wv->kick_ns = 0;

		wv_apply_retarget(wv);
		if (wv->target != NULL) {
			wv_tick(wv);
		}
		// The safety poll, or sooner when a dwell is about to cross. Never
		// faster while hidden: events bring the window back.
		now = os_monotonic_get_ns();
		next_ns = now + (uint64_t)U_WINDOW_VISIBILITY_PERIOD_MS * WV_MS_NS;
		const uint64_t dl = wv->target != NULL ? wv_next_deadline(wv) : 0;
		if (dl != 0 && dl < next_ns) {
			next_ns = dl;
		}
	}

	wv_events_stop(wv, &res);
	t_wv = NULL;
	if (set_dpi != NULL && prev != NULL) {
		set_dpi(prev);
	}
	return 0;
}

/*
 *
 * API.
 *
 */

DEBUG_GET_ONCE_BOOL_OPTION(lens_visibility, U_WINDOW_VISIBILITY_ENV_ENABLE, true)

struct u_window_visibility *
u_window_visibility_create(void *native_window,
                           const char *panel_device_name,
                           const char *who,
                           u_window_visibility_change_fn on_change,
                           void *userdata)
{
	if (native_window == NULL) {
		return NULL;
	}
	if (!debug_get_bool_option_lens_visibility()) {
		static bool logged = false;
		if (!logged) {
			logged = true;
			U_LOG_W("#2671: DXR_LENS_VISIBILITY=0 - hidden 3D windows keep the lens wish");
		}
		return NULL;
	}

	struct u_window_visibility *wv = (struct u_window_visibility *)calloc(1, sizeof(*wv));
	if (wv == NULL) {
		return NULL;
	}
	wv->target = (HWND)native_window;
	wv->on_change = on_change;
	wv->userdata = userdata;
	(void)snprintf(wv->who, sizeof(wv->who), "%s", who != NULL ? who : "window");
	InitializeSRWLock(&wv->panel_lock);
	(void)snprintf(wv->panel, sizeof(wv->panel), "%s", panel_device_name != NULL ? panel_device_name : "");
	debug_get_option(wv->extra_overlays, sizeof(wv->extra_overlays), U_WINDOW_VISIBILITY_ENV_OVERLAYS, "");
	wv->taskbar_auto_hide = -1;

	wv->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	wv->wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (wv->stop_event == NULL || wv->wake_event == NULL) {
		if (wv->stop_event != NULL) {
			CloseHandle(wv->stop_event);
		}
		if (wv->wake_event != NULL) {
			CloseHandle(wv->wake_event);
		}
		free(wv);
		return NULL;
	}
	wv->thread = CreateThread(NULL, 0, wv_thread, wv, 0, NULL);
	if (wv->thread == NULL) {
		CloseHandle(wv->stop_event);
		CloseHandle(wv->wake_event);
		free(wv);
		return NULL;
	}
	U_LOG_I("#2671 %s: tracking 3D window visibility (hwnd %p, 3D screen '%s'; tier 2 %s)", wv->who,
	        native_window, wv->panel, u_window_visibility_skip_present_enabled() ? "on" : "off");
	return wv;
}

bool
u_window_visibility_hidden(const struct u_window_visibility *wv)
{
	if (wv == NULL) {
		return false;
	}
	return InterlockedCompareExchange((volatile LONG *)&wv->hidden, 0, 0) != 0;
}

bool
u_window_visibility_hidden_everywhere(const struct u_window_visibility *wv)
{
	if (wv == NULL || !u_window_visibility_skip_present_enabled()) {
		return false;
	}
	return InterlockedCompareExchange((volatile LONG *)&wv->hidden_all, 0, 0) != 0;
}

void
u_window_visibility_get_state(const struct u_window_visibility *wv, struct u_window_visibility_state *out)
{
	memset(out, 0, sizeof(*out));
	if (wv == NULL) {
		return;
	}
	struct u_window_visibility *m = (struct u_window_visibility *)wv;
	AcquireSRWLockShared(&m->panel_lock);
	out->tracked = m->thread != NULL;
	out->reason = m->pub_reason;
	memcpy(out->occluder, m->pub_occluder, sizeof(out->occluder));
	out->occluder[sizeof(out->occluder) - 1] = '\0';
	out->since_ns = m->pub_since_ns;
	ReleaseSRWLockShared(&m->panel_lock);
	out->hidden = u_window_visibility_hidden(wv);
	out->hidden_everywhere = u_window_visibility_hidden_everywhere(wv);
	if (out->reason != U_WV_REASON_COVERED && out->reason != U_WV_REASON_PARTLY_COVERED) {
		out->occluder[0] = '\0';
	}
}

static void
wv_wake(struct u_window_visibility *wv)
{
	if (wv->wake_event != NULL) {
		SetEvent(wv->wake_event);
	}
}

void
u_window_visibility_set_panel(struct u_window_visibility *wv, const char *panel_device_name)
{
	if (wv == NULL) {
		return;
	}
	AcquireSRWLockExclusive(&wv->panel_lock);
	(void)snprintf(wv->panel, sizeof(wv->panel), "%s", panel_device_name != NULL ? panel_device_name : "");
	ReleaseSRWLockExclusive(&wv->panel_lock);
	wv_wake(wv);
}

void
u_window_visibility_set_window(struct u_window_visibility *wv, void *native_window)
{
	if (wv == NULL) {
		return;
	}
	AcquireSRWLockExclusive(&wv->panel_lock);
	wv->next_target = (HWND)native_window;
	wv->retarget = true;
	ReleaseSRWLockExclusive(&wv->panel_lock);
	// The thread applies it at its next tick; until then readers see the old
	// window's state, so clear a stale "hidden" now (the safe direction).
	InterlockedExchange(&wv->hidden_all, 0);
	InterlockedExchange(&wv->hidden, 0);
	wv_wake(wv);
}

void
u_window_visibility_set_screens(struct u_window_visibility *wv, const char *const *device_names, uint32_t count)
{
	if (wv == NULL) {
		return;
	}
	if (device_names == NULL) {
		count = 0;
	}
	if (count > U_WINDOW_VISIBILITY_MAX_SCREENS) {
		count = U_WINDOW_VISIBILITY_MAX_SCREENS;
	}
	AcquireSRWLockExclusive(&wv->panel_lock);
	bool same = count == wv->screen_count;
	for (uint32_t i = 0; same && i < count; i++) {
		same = strcmp(wv->screens[i].name, device_names[i] != NULL ? device_names[i] : "") == 0;
	}
	if (!same) {
		for (uint32_t i = 0; i < U_WINDOW_VISIBILITY_MAX_SCREENS; i++) {
			const char *nm = (i < count && device_names[i] != NULL) ? device_names[i] : "";
			(void)snprintf(wv->screens[i].name, sizeof(wv->screens[i].name), "%s", nm);
			InterlockedExchange(&wv->screens[i].hidden, 0);
		}
		wv->screen_count = count;
		wv->screens_gen++;
	}
	ReleaseSRWLockExclusive(&wv->panel_lock);
	if (!same) {
		U_LOG_I("#2671 %s: judging %u extra 3D screen(s) separately", wv->who, count);
		wv_wake(wv);
	}
}

bool
u_window_visibility_screen_hidden(const struct u_window_visibility *wv, const char *device_name)
{
	if (wv == NULL || device_name == NULL || device_name[0] == '\0') {
		return false;
	}
	// The lock is taken shared; the state it guards is not changed.
	struct u_window_visibility *m = (struct u_window_visibility *)wv;
	bool hidden = false;
	AcquireSRWLockShared(&m->panel_lock);
	for (uint32_t i = 0; i < m->screen_count; i++) {
		if (_stricmp(m->screens[i].name, device_name) == 0) {
			hidden = InterlockedCompareExchange(&m->screens[i].hidden, 0, 0) != 0;
			break;
		}
	}
	ReleaseSRWLockShared(&m->panel_lock);
	return hidden;
}

bool
u_window_visibility_get_region(const struct u_window_visibility *wv,
                               const char *device_name,
                               struct u_window_visibility_region *out)
{
	memset(out, 0, sizeof(*out));
	if (wv == NULL) {
		return false;
	}
	// The lock is taken shared; the state it guards is not changed.
	struct u_window_visibility *m = (struct u_window_visibility *)wv;
	bool found = true;
	AcquireSRWLockShared(&m->panel_lock);
	*out = m->pub_region;
	if (device_name != NULL && device_name[0] != '\0') {
		// Slot 0 is the session's 3D screen, slots 1.. the extra screens.
		int32_t slot = -1;
		bool hidden = false;
		if (m->panel[0] != '\0' && _stricmp(m->panel, device_name) == 0) {
			slot = 0;
			hidden = InterlockedCompareExchange(&m->hidden, 0, 0) != 0;
		}
		for (uint32_t i = 0; slot < 0 && i < m->screen_count; i++) {
			if (_stricmp(m->screens[i].name, device_name) == 0) {
				slot = (int32_t)(1 + i);
				hidden = InterlockedCompareExchange(&m->screens[i].hidden, 0, 0) != 0;
			}
		}
		if (slot < 0) {
			found = false;
		} else if (out->known && hidden) {
			memset(&out->region, 0, sizeof(out->region));
		} else if (out->known && m->pub_screen_found[slot]) {
			u_wv_region_clip(&out->region, &m->pub_screen_rect[slot], &out->region);
		}
	}
	ReleaseSRWLockShared(&m->panel_lock);
	if (!found) {
		memset(out, 0, sizeof(*out));
		return false;
	}
	return out->known;
}

uint64_t
u_window_visibility_region_gen(const struct u_window_visibility *wv)
{
	if (wv == NULL) {
		return 0;
	}
	return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)&wv->region_gen, 0, 0);
}

void
u_window_visibility_stop(struct u_window_visibility *wv)
{
	if (wv == NULL || wv->thread == NULL) {
		return;
	}
	SetEvent(wv->stop_event);
	// Joined, never detached: the thread must not outlive its owner. The
	// thread wakes on the event even inside its message wait.
	WaitForSingleObject(wv->thread, INFINITE);
	AcquireSRWLockExclusive(&wv->panel_lock);
	CloseHandle(wv->thread);
	wv->thread = NULL;
	ReleaseSRWLockExclusive(&wv->panel_lock);
	// Readers that still hold the pointer see "visible" from here on.
	wv_region_forget(wv);
	InterlockedExchange(&wv->hidden_all, 0);
	InterlockedExchange(&wv->hidden, 0);
	for (uint32_t i = 0; i < U_WINDOW_VISIBILITY_MAX_SCREENS; i++) {
		InterlockedExchange(&wv->screens[i].hidden, 0);
	}
}

void
u_window_visibility_destroy(struct u_window_visibility **wv_ptr)
{
	if (wv_ptr == NULL || *wv_ptr == NULL) {
		return;
	}
	struct u_window_visibility *wv = *wv_ptr;
	*wv_ptr = NULL;
	u_window_visibility_stop(wv);
	CloseHandle(wv->stop_event);
	CloseHandle(wv->wake_event);
	free(wv);
}
