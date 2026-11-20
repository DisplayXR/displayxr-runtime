// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Texture target drag phase-snap (ADR-060, #2725) — see the header.
 * @ingroup comp_util
 */

#include "xrt/xrt_config_os.h"

#include "util/comp_texture_target_drag_snap_win32.h"
#include "util/comp_texture_target_win32.h" // comp_texture_target_client_origin_win32

#include "util/u_logging.h"

#include <windows.h>
#include <commctrl.h> // SetWindowSubclass / RemoveWindowSubclass / DefSubclassProc

#include <stdlib.h>

/*
 *
 * Structs
 *
 */

/*!
 * One reference window. Refcounted: the session holds one reference, the
 * window side (pending install hook, then live subclass) the other.
 */
struct tt_binding
{
	volatile LONG refs;

	//! Guards fn / userdata / detached / active / target offset. The window
	//! thread holds it SHARED across a snap; detach takes it EXCLUSIVE.
	SRWLOCK lock;
	comp_tt_drag_snap_fn fn;
	void *userdata;
	bool detached;
	bool active;
	int32_t target_x;
	int32_t target_y;

	HWND ref;  //!< reference window (the target rect is in its client area)
	HWND root; //!< its top-level: runs the modal move loop, is subclassed
	DWORD tid; //!< root's thread
	const char *tier;

	//! Pending cross-thread install (under g_install_lock).
	HHOOK hook;
	bool pending;
	struct tt_binding *next_pending;

	//! Subclass live on root (set on the window thread; read anywhere).
	volatile LONG installed;

	//! Session-owned list link (session's threads only).
	struct tt_binding *next_owned;

	// Window thread only.
	bool in_move;
	bool move_ok;
	RECT frame0;
	int32_t client_dx; //!< ref client origin - root frame top-left, at drag start
	int32_t client_dy;
	bool live_logged;
	bool decline_logged;
	bool dpi_logged;
};

struct comp_tt_drag_snap
{
	comp_tt_drag_snap_fn fn;
	void *userdata;
	const char *tier;
	//! Binding for the current reference window (NULL: none / desktop px).
	struct tt_binding *cur;
	//! Every binding this session made that may still hold a window side.
	struct tt_binding *owned;
	bool armed_logged;
};


/*
 *
 * Globals
 *
 */

//! Pending cross-thread installs, consumed by the WH_GETMESSAGE hook.
static SRWLOCK g_install_lock = SRWLOCK_INIT;
static struct tt_binding *g_pending = NULL;

//! Registered once: "remove the subclass of binding wParam" (sent at destroy).
static UINT g_remove_msg = 0;
static INIT_ONCE g_remove_msg_once = INIT_ONCE_STATIC_INIT;

static volatile LONG g_pinned = 0;


/*
 *
 * Helpers
 *
 */

static BOOL CALLBACK
init_remove_msg(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	(void)once;
	(void)param;
	(void)ctx;
	g_remove_msg = RegisterWindowMessageW(L"DisplayXR.TextureTargetDragSnap.Remove");
	return TRUE;
}

static UINT
remove_msg(void)
{
	InitOnceExecuteOnce(&g_remove_msg_once, init_remove_msg, NULL, NULL);
	return g_remove_msg;
}

static void
binding_release(struct tt_binding *b)
{
	if (InterlockedDecrement(&b->refs) == 0) {
		free(b);
	}
}

typedef DPI_AWARENESS_CONTEXT(WINAPI *pfn_get_window_dpi_ctx)(HWND);
typedef DPI_AWARENESS(WINAPI *pfn_get_awareness_from_ctx)(DPI_AWARENESS_CONTEXT);

/*!
 * Are @p hwnd's message coordinates physical device px (per-monitor aware)?
 * A DPI-virtualised window would hand us logical px, which the DP's lattice
 * is not in — such a window is left unsnapped. Before Windows 10 1607 there
 * is no per-window context to ask, and no virtualisation of this kind.
 */
static bool
window_is_per_monitor_aware(HWND hwnd)
{
	static pfn_get_window_dpi_ctx get_ctx = NULL;
	static pfn_get_awareness_from_ctx get_aw = NULL;
	static volatile LONG looked = 0;
	if (InterlockedCompareExchange(&looked, 0, 0) == 0) {
		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		if (user32 != NULL) {
			get_ctx =
			    (pfn_get_window_dpi_ctx)(void *)GetProcAddress(user32, "GetWindowDpiAwarenessContext");
			get_aw = (pfn_get_awareness_from_ctx)(void *)GetProcAddress(
			    user32, "GetAwarenessFromDpiAwarenessContext");
		}
		InterlockedExchange(&looked, 1);
	}
	if (get_ctx == NULL || get_aw == NULL) {
		return true;
	}
	DPI_AWARENESS_CONTEXT ctx = get_ctx(hwnd);
	return ctx != NULL && get_aw(ctx) == DPI_AWARENESS_PER_MONITOR_AWARE;
}

static void
pin_module_once(const char *tier)
{
	if (InterlockedExchange(&g_pinned, 1) != 0) {
		return;
	}
	HMODULE self = NULL;
	(void)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
	                         (LPCWSTR)(void *)&pin_module_once, &self);
	U_LOG_W(
	    "ADR-060 (#2725, %s): a reference window's thread did not answer at teardown — its drag-snap subclass is "
	    "left inert and the runtime module stays loaded for the life of the process",
	    tier);
}


/*
 *
 * Window thread
 *
 */

static LRESULT CALLBACK
subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);

//! Window thread: remove the subclass and drop the window-side reference.
static void
uninstall_on_window_thread(struct tt_binding *b, HWND hwnd)
{
	if (InterlockedExchange(&b->installed, 0) != 0) {
		RemoveWindowSubclass(hwnd, subclass_proc, (UINT_PTR)b);
		binding_release(b);
	}
}

static void
drag_begin(struct tt_binding *b, HWND hwnd)
{
	b->in_move = true;
	b->move_ok = false;
	if (!window_is_per_monitor_aware(hwnd)) {
		if (!b->dpi_logged) {
			b->dpi_logged = true;
			U_LOG_W(
			    "ADR-060 (#2725, %s): reference window %p is not per-monitor DPI aware — its drags are "
			    "not phase-snapped (its coordinates are not device px)",
			    b->tier, (void *)hwnd);
		}
		return;
	}
	POINT client = {0, 0};
	if (!GetWindowRect(hwnd, &b->frame0) || !ClientToScreen(b->ref, &client)) {
		return;
	}
	b->client_dx = (int32_t)client.x - (int32_t)b->frame0.left;
	b->client_dy = (int32_t)client.y - (int32_t)b->frame0.top;
	b->move_ok = true;
}

static void
drag_step(struct tt_binding *b, WINDOWPOS *pos)
{
	if (!b->in_move || !b->move_ok || pos == NULL || (pos->flags & SWP_NOMOVE) != 0) {
		return;
	}
	// A pure move only: a resize changes the client area the target is in.
	const int32_t w0 = (int32_t)(b->frame0.right - b->frame0.left);
	const int32_t h0 = (int32_t)(b->frame0.bottom - b->frame0.top);
	if ((pos->flags & SWP_NOSIZE) == 0 && (pos->cx != w0 || pos->cy != h0)) {
		return;
	}

	AcquireSRWLockShared(&b->lock);
	if (!b->detached && b->fn != NULL && b->active) {
		// Evaluate at the TARGET's present origin: frame + client offset +
		// target offset in the client area. The offset is constant over a
		// move, so the delta found there is the delta for the frame.
		const int32_t kx = b->client_dx + b->target_x;
		const int32_t ky = b->client_dy + b->target_y;
		const int32_t ox = (int32_t)b->frame0.left + kx;
		const int32_t oy = (int32_t)b->frame0.top + ky;
		const int32_t tx = (int32_t)pos->x + kx;
		const int32_t ty = (int32_t)pos->y + ky;
		int32_t sx = tx;
		int32_t sy = ty;
		if (b->fn(b->userdata, ox, oy, tx, ty, &sx, &sy)) {
			const int32_t dx = sx - tx;
			const int32_t dy = sy - ty;
			if (dx != 0 || dy != 0) {
				pos->x += dx;
				pos->y += dy;
				if (!b->live_logged) {
					b->live_logged = true;
					U_LOG_W(
					    "ADR-060 (#2725, %s): texture target drag snap LIVE (first correction "
					    "(%d,%d))",
					    b->tier, (int)dx, (int)dy);
				}
			}
		} else if (!b->decline_logged) {
			b->decline_logged = true;
			U_LOG_W(
			    "ADR-060 (#2725, %s): the display processor declined a texture target drag snap — the "
			    "reference window moves unsnapped",
			    b->tier);
		}
	}
	ReleaseSRWLockShared(&b->lock);
}

static LRESULT CALLBACK
subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
	(void)id;
	struct tt_binding *b = (struct tt_binding *)data;

	if (msg == WM_NCDESTROY) {
		// The app destroyed its window first: the subclass must be gone
		// before the window is.
		uninstall_on_window_thread(b, hwnd);
		return DefSubclassProc(hwnd, msg, wp, lp);
	}
	if (msg == remove_msg() && (struct tt_binding *)wp == b) {
		uninstall_on_window_thread(b, hwnd);
		return 1;
	}

	AcquireSRWLockShared(&b->lock);
	const bool detached = b->detached;
	ReleaseSRWLockShared(&b->lock);
	if (detached) {
		// Unbound or rebound elsewhere: leave on the first message we see.
		uninstall_on_window_thread(b, hwnd);
		return DefSubclassProc(hwnd, msg, wp, lp);
	}

	switch (msg) {
	case WM_ENTERSIZEMOVE: drag_begin(b, hwnd); break;
	case WM_EXITSIZEMOVE: b->in_move = false; break;
	case WM_WINDOWPOSCHANGING: {
		// The app's own handling first: what it proposes is what is snapped.
		const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
		drag_step(b, (WINDOWPOS *)lp);
		return r;
	}
	default: break;
	}
	return DefSubclassProc(hwnd, msg, wp, lp);
}

//! Window thread (or the caller when it IS that thread): install the subclass.
static void
install_on_window_thread(struct tt_binding *b)
{
	bool detached;
	AcquireSRWLockShared(&b->lock);
	detached = b->detached;
	ReleaseSRWLockShared(&b->lock);
	if (detached || !IsWindow(b->root)) {
		binding_release(b); // the window side never came to be
		return;
	}
	InterlockedExchange(&b->installed, 1);
	if (!SetWindowSubclass(b->root, subclass_proc, (UINT_PTR)b, (DWORD_PTR)b)) {
		InterlockedExchange(&b->installed, 0);
		U_LOG_W(
		    "ADR-060 (#2725, %s): could not subclass reference window %p (error %lu) — drags are not snapped",
		    b->tier, (void *)b->root, GetLastError());
		binding_release(b);
	}
}

static LRESULT CALLBACK
getmsg_hook(int code, WPARAM wp, LPARAM lp)
{
	const DWORD tid = GetCurrentThreadId();
	AcquireSRWLockExclusive(&g_install_lock);
	struct tt_binding **pp = &g_pending;
	while (*pp != NULL) {
		struct tt_binding *b = *pp;
		if (b->tid != tid) {
			pp = &b->next_pending;
			continue;
		}
		*pp = b->next_pending;
		b->next_pending = NULL;
		b->pending = false;
		HHOOK hook = b->hook;
		b->hook = NULL;
		if (hook != NULL) {
			UnhookWindowsHookEx(hook); // allowed from inside the hook itself
		}
		install_on_window_thread(b);
	}
	ReleaseSRWLockExclusive(&g_install_lock);
	return CallNextHookEx(NULL, code, wp, lp);
}


/*
 *
 * Session side
 *
 */

static struct tt_binding *
binding_create(struct comp_tt_drag_snap *s, HWND ref, int32_t target_x, int32_t target_y)
{
	HWND root = GetAncestor(ref, GA_ROOT);
	if (root == NULL) {
		root = ref;
	}
	DWORD pid = 0;
	const DWORD tid = GetWindowThreadProcessId(root, &pid);
	if (tid == 0 || pid != GetCurrentProcessId()) {
		U_LOG_W("ADR-060 (#2725, %s): reference window %p is not a live window of this process — no drag snap",
		        s->tier, (void *)ref);
		return NULL;
	}

	struct tt_binding *b = (struct tt_binding *)calloc(1, sizeof(*b));
	if (b == NULL) {
		return NULL;
	}
	b->refs = 2; // session + window side
	InitializeSRWLock(&b->lock);
	b->fn = s->fn;
	b->userdata = s->userdata;
	b->target_x = target_x;
	b->target_y = target_y;
	b->ref = ref;
	b->root = root;
	b->tid = tid;
	b->tier = s->tier;
	(void)remove_msg(); // registered before the window thread first compares against it

	b->next_owned = s->owned;
	s->owned = b;

	if (tid == GetCurrentThreadId()) {
		install_on_window_thread(b);
		return b;
	}
	AcquireSRWLockExclusive(&g_install_lock);
	b->hook = SetWindowsHookExW(WH_GETMESSAGE, getmsg_hook, NULL, tid);
	if (b->hook == NULL) {
		ReleaseSRWLockExclusive(&g_install_lock);
		U_LOG_W("ADR-060 (#2725, %s): could not reach reference window %p's thread (error %lu) — no drag snap",
		        s->tier, (void *)root, GetLastError());
		binding_release(b); // window side
		return b;
	}
	b->pending = true;
	b->next_pending = g_pending;
	g_pending = b;
	ReleaseSRWLockExclusive(&g_install_lock);
	// The hook runs when the thread retrieves a message: give it one.
	PostMessageW(root, WM_NULL, 0, 0);
	return b;
}

//! Detach (exclusive: waits out a snap in flight) and cancel a pending install.
static void
binding_detach(struct tt_binding *b)
{
	AcquireSRWLockExclusive(&b->lock);
	b->detached = true;
	b->fn = NULL;
	b->userdata = NULL;
	ReleaseSRWLockExclusive(&b->lock);

	AcquireSRWLockExclusive(&g_install_lock);
	if (b->pending) {
		for (struct tt_binding **pp = &g_pending; *pp != NULL; pp = &(*pp)->next_pending) {
			if (*pp == b) {
				*pp = b->next_pending;
				break;
			}
		}
		b->pending = false;
		b->next_pending = NULL;
		if (b->hook != NULL) {
			UnhookWindowsHookEx(b->hook);
			b->hook = NULL;
		}
		binding_release(b); // window side: never installed
	}
	ReleaseSRWLockExclusive(&g_install_lock);
}

//! Drop owned bindings whose window side is gone (session reference only).
static void
prune_owned(struct comp_tt_drag_snap *s)
{
	struct tt_binding **pp = &s->owned;
	while (*pp != NULL) {
		struct tt_binding *b = *pp;
		if (b != s->cur && InterlockedCompareExchange(&b->refs, 0, 0) == 1) {
			*pp = b->next_owned;
			binding_release(b);
			continue;
		}
		pp = &b->next_owned;
	}
}

struct comp_tt_drag_snap *
comp_tt_drag_snap_create(
    void *ref_window, int32_t target_x, int32_t target_y, comp_tt_drag_snap_fn fn, void *userdata, const char *tier)
{
	if (fn == NULL) {
		return NULL;
	}
	struct comp_tt_drag_snap *s = (struct comp_tt_drag_snap *)calloc(1, sizeof(*s));
	if (s == NULL) {
		return NULL;
	}
	s->fn = fn;
	s->userdata = userdata;
	s->tier = tier != NULL ? tier : "?";

	// Prewarm one snap from THIS thread (the compositor's), at the target's
	// present origin — a probe of availability, logged once.
	const char *prewarm = "no reference window yet";
	POINT client = {0, 0};
	if (ref_window != NULL && comp_texture_target_client_origin_win32(ref_window, &client, NULL)) {
		const int32_t px = (int32_t)client.x + target_x;
		const int32_t py = (int32_t)client.y + target_y;
		int32_t sx = px;
		int32_t sy = py;
		prewarm = fn(userdata, px, py, px, py, &sx, &sy) ? "snap available" : "declined";
	}
	if (ref_window != NULL) {
		s->cur = binding_create(s, (HWND)ref_window, target_x, target_y);
	}
	U_LOG_W(
	    "ADR-060 (#2725, %s): texture target drag snap armed on reference window %p (prewarm: %s) — user drags "
	    "of it are phase-snapped at the target's present origin while it is woven windowless",
	    s->tier, ref_window, prewarm);
	return s;
}

void
comp_tt_drag_snap_update(struct comp_tt_drag_snap *s, void *ref_window, int32_t target_x, int32_t target_y, bool active)
{
	if (s == NULL) {
		return;
	}
	if ((s->cur == NULL ? NULL : (void *)s->cur->ref) != ref_window) {
		// Dock / undock / full screen: the target moved to another window.
		if (s->cur != NULL) {
			binding_detach(s->cur);
			if (InterlockedCompareExchange(&s->cur->installed, 0, 0) != 0) {
				// Its window thread removes it on the next message.
				PostMessageW(s->cur->root, WM_NULL, 0, 0);
			}
			s->cur = NULL;
		}
		prune_owned(s);
		if (ref_window != NULL && IsWindow((HWND)ref_window)) {
			s->cur = binding_create(s, (HWND)ref_window, target_x, target_y);
			U_LOG_W("ADR-060 (#2725, %s): texture target drag snap moved to reference window %p", s->tier,
			        ref_window);
		}
	}
	struct tt_binding *b = s->cur;
	if (b == NULL) {
		return;
	}
	AcquireSRWLockExclusive(&b->lock);
	b->target_x = target_x;
	b->target_y = target_y;
	b->active = active;
	ReleaseSRWLockExclusive(&b->lock);
}

void
comp_tt_drag_snap_destroy(struct comp_tt_drag_snap **s_ptr)
{
	if (s_ptr == NULL || *s_ptr == NULL) {
		return;
	}
	struct comp_tt_drag_snap *s = *s_ptr;
	*s_ptr = NULL;

	// First detach everything: from here on no DP call is in flight or to come.
	for (struct tt_binding *b = s->owned; b != NULL; b = b->next_owned) {
		binding_detach(b);
	}
	// Then remove the subclasses where the window thread can be reached.
	struct tt_binding *b = s->owned;
	while (b != NULL) {
		struct tt_binding *next = b->next_owned;
		if (InterlockedCompareExchange(&b->installed, 0, 0) != 0) {
			if (b->tid == GetCurrentThreadId()) {
				uninstall_on_window_thread(b, b->root);
			} else {
				DWORD_PTR res = 0;
				(void)SendMessageTimeoutW(b->root, remove_msg(), (WPARAM)b, 0,
				                          SMTO_ABORTIFHUNG | SMTO_NORMAL, 200, &res);
				if (InterlockedCompareExchange(&b->installed, 0, 0) != 0) {
					// Inert (detached) until its next message removes it; the
					// window must never call into an unloaded module.
					PostMessageW(b->root, WM_NULL, 0, 0);
					pin_module_once(s->tier);
				}
			}
		}
		binding_release(b); // session reference
		b = next;
	}
	free(s);
}
