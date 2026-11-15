// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window visibility for the lens wish (#2671): the Windows tracker.
 * @ingroup aux_util
 *
 * One thread per tracked window. Every check runs with the thread pinned to
 * per-monitor-v2 DPI awareness, so every rect is physical whatever the host
 * process declared (docs/reference/dpi-awareness.md: this code runs inside
 * the app's process on the in-process routes).
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

//! Most windows walked above the target per check (bounds a pathological desktop).
#define WV_MAX_WALK 1024
//! Most occluders kept per check; a cover needs far fewer.
#define WV_MAX_OCCLUDERS 256
//! Process-name cache size and lifetime (pid reuse).
#define WV_PID_CACHE 32
#define WV_PID_CACHE_TTL_NS (10ull * 1000ull * 1000ull * 1000ull)

struct wv_pid_entry
{
	DWORD pid;
	char name[MAX_PATH];
};

struct u_window_visibility
{
	HWND target;
	char who[64];
	u_window_visibility_change_fn on_change;
	void *userdata;
	char extra_overlays[512];

	//! Guards @ref panel, @ref next_target and @ref retarget.
	SRWLOCK panel_lock;
	char panel[128];
	HWND next_target;
	bool retarget;

	//! Debounced result, read by any thread.
	volatile LONG hidden;

	HANDLE stop_event;
	HANDLE thread;

	// Thread-private state below.
	struct u_wv_debounce debounce;
	DWORD target_pid;
	const char *raw_reason;
	struct wv_pid_entry pids[WV_PID_CACHE];
	uint32_t pid_count;
	uint64_t pid_cache_ns;
	struct u_wv_rect occ[WV_MAX_OCCLUDERS];
};

// Resolved dynamically, as os_display_desktop_win32.c does: Windows 10 1607+,
// and MinGW headers may not declare it. HANDLE stands in for DPI_AWARENESS_CONTEXT.
typedef HANDLE wv_dpi_context_t;
typedef wv_dpi_context_t(WINAPI *wv_set_thread_dpi_fn)(wv_dpi_context_t);
#define WV_DPI_PER_MONITOR_AWARE_V2 ((wv_dpi_context_t)(intptr_t)-4)

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

struct wv_monitor_find
{
	const char *name;
	RECT rect;
	bool found;
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
	if (GetMonitorInfoA(mon, (LPMONITORINFO)&mi) && _stricmp(mi.szDevice, f->name) == 0) {
		f->rect = mi.rcMonitor;
		f->found = true;
		return FALSE;
	}
	return TRUE;
}

//! The 3D screen's physical rect, by GDI device name. False when unknown.
static bool
wv_panel_rect(struct u_window_visibility *wv, struct u_wv_rect *out)
{
	char name[sizeof(wv->panel)];
	AcquireSRWLockShared(&wv->panel_lock);
	memcpy(name, wv->panel, sizeof(name));
	ReleaseSRWLockShared(&wv->panel_lock);
	if (name[0] == '\0') {
		return false;
	}
	struct wv_monitor_find f = {name, {0, 0, 0, 0}, false};
	EnumDisplayMonitors(NULL, NULL, wv_monitor_cb, (LPARAM)&f);
	if (!f.found) {
		return false;
	}
	*out = wv_from_rect(&f.rect);
	return true;
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

/*
 *
 * The check.
 *
 */

//! One raw check: true when the target is hidden now. Sets wv->raw_reason.
static bool
wv_check(struct u_window_visibility *wv)
{
	HWND target = wv->target;
	if (!IsWindow(target)) {
		wv->raw_reason = "window gone";
		return false;
	}
	HWND root = GetAncestor(target, GA_ROOT);
	if (root == NULL) {
		root = target;
	}
	if (IsIconic(root)) {
		wv->raw_reason = "minimized";
		return true;
	}
	if (!IsWindowVisible(root) || !IsWindowVisible(target)) {
		wv->raw_reason = "not visible";
		return true;
	}
	if (wv_cloaked(root)) {
		wv->raw_reason = "cloaked";
		return true;
	}

	RECT cr;
	if (!GetClientRect(target, &cr)) {
		wv->raw_reason = "no client rect";
		return false;
	}
	POINT tl = {cr.left, cr.top};
	POINT br = {cr.right, cr.bottom};
	ClientToScreen(target, &tl);
	ClientToScreen(target, &br);
	struct u_wv_rect t = {tl.x, tl.y, br.x, br.y};

	struct u_wv_rect panel;
	if (wv_panel_rect(wv, &panel)) {
		t = u_wv_rect_intersect(&t, &panel);
	}
	if (u_wv_rect_empty(&t)) {
		// Nothing on the 3D screen: the multi-screen coverage gate owns that case.
		wv->raw_reason = "off the 3D screen";
		return false;
	}

	if (wv->target_pid == 0) {
		GetWindowThreadProcessId(root, &wv->target_pid);
	}
	const DWORD self_pid = GetCurrentProcessId();

	size_t n = 0;
	int walked = 0;
	for (HWND w = GetWindow(root, GW_HWNDPREV); w != NULL && walked < WV_MAX_WALK;
	     w = GetWindow(w, GW_HWNDPREV), walked++) {
		if (!IsWindowVisible(w) || IsIconic(w)) {
			continue;
		}
		struct u_wv_rect fb;
		if (!wv_frame_bounds(w, &fb)) {
			continue;
		}
		const struct u_wv_rect x = u_wv_rect_intersect(&fb, &t);
		if (u_wv_rect_empty(&x)) {
			continue;
		}

		DWORD pid = 0;
		GetWindowThreadProcessId(w, &pid);
		char cls[128];
		cls[0] = '\0';
		GetClassNameA(w, cls, (int)sizeof(cls));
		const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);

		struct u_wv_window_facts f;
		memset(&f, 0, sizeof(f));
		f.class_name = cls;
		f.visible = true;
		f.same_process = pid == wv->target_pid || pid == self_pid;
		f.cloaked = wv_cloaked(w);
		f.no_redirection_bitmap =
		    (ex & WS_EX_NOREDIRECTIONBITMAP) != 0 && strcmp(cls, "ApplicationFrameWindow") == 0;
		f.input_transparent = (ex & WS_EX_TRANSPARENT) != 0;
		f.layered_not_opaque = wv_layered_not_opaque(w, ex);
		// Only open the process when nothing cheaper already decided.
		f.process_name = "";
		if (u_wv_classify_occluder(&f, NULL) == U_WV_OCCLUDES) {
			f.process_name = wv_process_name(wv, pid);
		}
		if (u_wv_classify_occluder(&f, wv->extra_overlays) != U_WV_OCCLUDES) {
			continue;
		}
		if (n < WV_MAX_OCCLUDERS) {
			wv->occ[n++] = x;
		}
	}

	if (u_wv_rect_covered(&t, wv->occ, n)) {
		wv->raw_reason = "fully covered";
		return true;
	}
	wv->raw_reason = n > 0 ? "partly covered" : "uncovered";
	return false;
}

static DWORD WINAPI
wv_thread(LPVOID ptr)
{
	struct u_window_visibility *wv = (struct u_window_visibility *)ptr;

	wv_dpi_context_t prev = NULL;
	wv_set_thread_dpi_fn set_dpi = NULL;
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	if (user32 != NULL) {
		set_dpi = (wv_set_thread_dpi_fn)(void *)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
	}
	if (set_dpi != NULL) {
		prev = set_dpi(WV_DPI_PER_MONITOR_AWARE_V2);
	}

	DWORD period = U_WINDOW_VISIBILITY_PERIOD_VISIBLE_MS;
	while (WaitForSingleObject(wv->stop_event, period) == WAIT_TIMEOUT) {
		AcquireSRWLockExclusive(&wv->panel_lock);
		const bool retarget = wv->retarget;
		if (retarget) {
			wv->retarget = false;
			wv->target = wv->next_target;
		}
		ReleaseSRWLockExclusive(&wv->panel_lock);
		if (retarget) {
			// A new window starts visible: nothing is known about it yet.
			wv->target_pid = 0;
			memset(&wv->debounce, 0, sizeof(wv->debounce));
			if (InterlockedExchange(&wv->hidden, 0) != 0) {
				U_LOG_W("#2671 %s: tracking a new 3D window — lens wish back to the session's mode",
				        wv->who);
				if (wv->on_change != NULL) {
					wv->on_change(wv->userdata, false);
				}
			}
		}
		if (wv->target == NULL) {
			period = U_WINDOW_VISIBILITY_PERIOD_VISIBLE_MS;
			continue;
		}
		const bool raw = wv_check(wv);
		const bool was = wv->debounce.reported_hidden;
		const bool now =
		    u_wv_debounce_update(&wv->debounce, raw, os_monotonic_get_ns(), U_WINDOW_VISIBILITY_HIDE_DWELL_NS);
		if (now != was) {
			InterlockedExchange(&wv->hidden, now ? 1 : 0);
			U_LOG_W("#2671 %s: the 3D window is %s (%s) — lens wish %s", wv->who,
			        now ? "hidden" : "visible again", wv->raw_reason != NULL ? wv->raw_reason : "?",
			        now ? "2D unless another 3D window is visible" : "back to the session's mode");
			if (wv->on_change != NULL) {
				wv->on_change(wv->userdata, now);
			}
		}
		period = raw ? U_WINDOW_VISIBILITY_PERIOD_HIDDEN_MS : U_WINDOW_VISIBILITY_PERIOD_VISIBLE_MS;
	}

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

DEBUG_GET_ONCE_BOOL_OPTION(lens_visibility, "DXR_LENS_VISIBILITY", true)

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
			U_LOG_W("#2671: DXR_LENS_VISIBILITY=0 — hidden 3D windows keep the lens wish");
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

	wv->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (wv->stop_event == NULL) {
		free(wv);
		return NULL;
	}
	wv->thread = CreateThread(NULL, 0, wv_thread, wv, 0, NULL);
	if (wv->thread == NULL) {
		CloseHandle(wv->stop_event);
		free(wv);
		return NULL;
	}
	U_LOG_I("#2671 %s: tracking 3D window visibility (hwnd %p, 3D screen '%s')", wv->who, native_window, wv->panel);
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

void
u_window_visibility_set_panel(struct u_window_visibility *wv, const char *panel_device_name)
{
	if (wv == NULL) {
		return;
	}
	AcquireSRWLockExclusive(&wv->panel_lock);
	(void)snprintf(wv->panel, sizeof(wv->panel), "%s", panel_device_name != NULL ? panel_device_name : "");
	ReleaseSRWLockExclusive(&wv->panel_lock);
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
	InterlockedExchange(&wv->hidden, 0);
}

void
u_window_visibility_destroy(struct u_window_visibility **wv_ptr)
{
	if (wv_ptr == NULL || *wv_ptr == NULL) {
		return;
	}
	struct u_window_visibility *wv = *wv_ptr;
	*wv_ptr = NULL;
	SetEvent(wv->stop_event);
	// Joined, never detached: the thread must not outlive its owner.
	WaitForSingleObject(wv->thread, INFINITE);
	CloseHandle(wv->thread);
	CloseHandle(wv->stop_event);
	free(wv);
}
