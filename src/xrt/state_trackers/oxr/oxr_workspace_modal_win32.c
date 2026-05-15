// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tier 0 modal-dialog re-parenting for the workspace shell (GH #227).
 *
 * When `DISPLAYXR_WORKSPACE_SESSION=1` and the runtime hides the app's HWND
 * (oxr_session.c ShowWindow(SW_HIDE) site), modal popups the app spawns
 * (`GetOpenFileName`, `MessageBox`, the in-process portion of
 * `IFileOpenDialog`, …) end up owned by a hidden window — z-order behind the
 * shell's fullscreen swap chain, focus return broken, taskbar chain broken.
 *
 * This module installs a per-thread `WH_CBT` hook on the OpenXR session's
 * UI thread. The hook handles `HCBT_CREATEWND` synchronously, before the
 * window is materialized, and rewrites `lpcs->hwndParent` from the hidden
 * app HWND to a visible offscreen "dialog owner" window we own. That
 * restores the entire ownership chain Windows uses for modal disable / re-
 * enable / focus restoration / taskbar grouping.
 *
 * `WH_CBT` over `SetWinEventHook(EVENT_OBJECT_CREATE)` is essential —
 * EVENT_OBJECT_CREATE fires *after* the dialog has already cached the
 * (hidden) owner for `EnableWindow`-style modal disable, so post-hoc owner
 * rewrites leave focus restoration broken.
 *
 * Coverage matrix in docs/specs/runtime/modal-dialog-handling.md. COM-based
 * `IFileOpenDialog` workers run on threads we don't hook unless their
 * creation falls through `DLL_THREAD_ATTACH`; partial coverage acknowledged.
 *
 * Tied to the runtime via two new RPCs:
 * - `session_set_modal_state(is_open)` — IPC notification on 0↔1 transition
 * - workspace event channel emits `IPC_WORKSPACE_INPUT_EVENT_MODAL_OPEN /
 *   _CLOSE` so the shell can dim, drop swap-chain topmost, 3D→2D toggle.
 *
 * Workspace mode is one OpenXR session per app process, so all state in
 * this file is process-global. `init` is idempotent across session
 * recreate; `fini` tears down. Not thread-safe across init/fini races
 * (init/fini happen on the OXR session create/destroy thread, which is
 * single-threaded for any given session).
 *
 * @ingroup oxr_main
 */

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_WINDOWS

#include "oxr_objects.h"

#include "util/u_logging.h"
#include "xrt/xrt_results.h"

#include <windows.h>
#include <stdbool.h>
#include <string.h>

// Bridge to the IPC client compositor — declared here rather than via
// ipc_client.h because st_oxr doesn't pull the ipc_client include path.
// The runtime DLL links ipc_client so the symbol resolves at link time.
// Same pattern as comp_ipc_client_compositor_get_window_metrics in
// oxr_session.c.
struct xrt_compositor;
extern xrt_result_t
comp_ipc_client_compositor_session_set_modal_state(struct xrt_compositor *xc, bool is_open);

#define MODAL_OWNER_CLASS L"DisplayXRModalDialogOwner"
#define MAX_TRACKED_DIALOGS 16

static HWND s_app_hidden_hwnd = NULL;
static HWND s_dialog_owner_hwnd = NULL;
static HHOOK s_cbt_hook = NULL;
static struct xrt_compositor *s_workspace_xc = NULL;
static int s_modal_open_depth = 0;
static HWND s_tracked_dialogs[MAX_TRACKED_DIALOGS];
static int s_tracked_dialog_count = 0;
static CRITICAL_SECTION s_lock;
static bool s_lock_initialized = false;
static bool s_class_registered = false;

static void
notify_modal_state_locked(bool is_open)
{
	// Critical-section held by caller. The IPC call itself does not
	// require our lock; we only hold it long enough to read s_workspace_xc
	// without a torn pointer. Do not call IPC under the critical section
	// — release first.
	struct xrt_compositor *xc = s_workspace_xc;
	if (xc == NULL) {
		return;
	}
	LeaveCriticalSection(&s_lock);
	(void)comp_ipc_client_compositor_session_set_modal_state(xc, is_open);
	EnterCriticalSection(&s_lock);
}

static bool
track_dialog(HWND hwnd)
{
	for (int i = 0; i < s_tracked_dialog_count; i++) {
		if (s_tracked_dialogs[i] == hwnd) return false; // already tracked
	}
	if (s_tracked_dialog_count >= MAX_TRACKED_DIALOGS) {
		// Drop on the floor — depth count goes out of sync but it's
		// preferable to overflowing the array. Logged once per init.
		U_LOG_W("workspace_modal: tracked dialog array full (%d) — depth refcount may drift",
		        MAX_TRACKED_DIALOGS);
		return false;
	}
	s_tracked_dialogs[s_tracked_dialog_count++] = hwnd;
	return true;
}

static bool
untrack_dialog(HWND hwnd)
{
	for (int i = 0; i < s_tracked_dialog_count; i++) {
		if (s_tracked_dialogs[i] == hwnd) {
			s_tracked_dialogs[i] = s_tracked_dialogs[--s_tracked_dialog_count];
			return true;
		}
	}
	return false;
}

static LRESULT CALLBACK
cbt_hook_proc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code < 0) {
		return CallNextHookEx(NULL, code, wParam, lParam);
	}

	if (code == HCBT_CREATEWND && s_app_hidden_hwnd != NULL && s_dialog_owner_hwnd != NULL) {
		HWND new_hwnd = (HWND)wParam;
		CBT_CREATEWND *cbt = (CBT_CREATEWND *)lParam;
		if (cbt != NULL && cbt->lpcs != NULL && cbt->lpcs->hwndParent == s_app_hidden_hwnd &&
		    new_hwnd != s_dialog_owner_hwnd) {
			// Re-parent in place. Filtering on parent==app_hidden_hwnd
			// is deterministic: child controls (parent is the dialog)
			// and unrelated top-levels (parent is NULL or some other
			// HWND) are left alone. No class allowlist needed.
			cbt->lpcs->hwndParent = s_dialog_owner_hwnd;

			EnterCriticalSection(&s_lock);
			bool was_first = (s_modal_open_depth == 0);
			if (track_dialog(new_hwnd)) {
				s_modal_open_depth++;
			}
			if (was_first) {
				notify_modal_state_locked(true);
			}
			LeaveCriticalSection(&s_lock);
		}
	} else if (code == HCBT_DESTROYWND) {
		HWND hwnd = (HWND)wParam;
		EnterCriticalSection(&s_lock);
		if (untrack_dialog(hwnd)) {
			s_modal_open_depth--;
			if (s_modal_open_depth <= 0) {
				s_modal_open_depth = 0;
				notify_modal_state_locked(false);
			}
		}
		LeaveCriticalSection(&s_lock);
	}

	return CallNextHookEx(NULL, code, wParam, lParam);
}

static bool
ensure_class_registered(void)
{
	if (s_class_registered) {
		return true;
	}
	WNDCLASSEXW wc;
	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = MODAL_OWNER_CLASS;
	if (RegisterClassExW(&wc) == 0) {
		DWORD err = GetLastError();
		if (err != ERROR_CLASS_ALREADY_EXISTS) {
			U_LOG_W("workspace_modal: RegisterClassExW failed: 0x%08lx", err);
			return false;
		}
	}
	s_class_registered = true;
	return true;
}

void
oxr_workspace_modal_win32_init(struct oxr_session *sess, void *app_hwnd)
{
	if (sess == NULL || app_hwnd == NULL) {
		return;
	}
	if (!s_lock_initialized) {
		InitializeCriticalSection(&s_lock);
		s_lock_initialized = true;
	}

	EnterCriticalSection(&s_lock);
	if (s_cbt_hook != NULL) {
		// Idempotent: already initialized for a previous session in
		// this process. The plan-document scenario (LOSS_PENDING-driven
		// session recreate while still in workspace mode) reuses the
		// existing hook + dialog owner. If app_hwnd changed (rare —
		// would mean the app destroyed its top-level and made a new
		// one), update the tracked HWND.
		s_app_hidden_hwnd = (HWND)app_hwnd;
		s_workspace_xc = (sess->xcn != NULL) ? &sess->xcn->base : NULL;
		LeaveCriticalSection(&s_lock);
		return;
	}
	LeaveCriticalSection(&s_lock);

	if (!ensure_class_registered()) {
		return;
	}

	// Offscreen visible owner. WS_VISIBLE so Windows treats it as a real
	// owner candidate for modal disable/re-enable; WS_EX_TOOLWINDOW keeps
	// it out of the taskbar; WS_EX_NOACTIVATE so creation doesn't steal
	// focus; WS_EX_LAYERED + alpha 0 so it can't be seen if any user
	// somehow drags a window aside; positioned far offscreen.
	HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
	                             MODAL_OWNER_CLASS, L"DisplayXR Modal Dialog Owner",
	                             WS_POPUP | WS_VISIBLE,
	                             -32000, -32000, 1, 1, NULL, NULL,
	                             GetModuleHandleW(NULL), NULL);
	if (owner == NULL) {
		U_LOG_W("workspace_modal: CreateWindowExW(owner) failed: 0x%08lx", GetLastError());
		return;
	}
	(void)SetLayeredWindowAttributes(owner, 0, 0, LWA_ALPHA);

	// Per-thread CBT hook — fires synchronously on HCBT_CREATEWND so we
	// can mutate lpcs->hwndParent in place before the dialog disables its
	// owner. Per-thread is correct: dialogs spawn on whatever thread
	// called CreateWindowEx; OXR session-create runs on the app's UI
	// thread which is also where modal popups normally come from. COM
	// workers (IFileOpenDialog) run on separate threads — partial
	// coverage acknowledged in the spec doc.
	HHOOK hook = SetWindowsHookExW(WH_CBT, cbt_hook_proc, NULL, GetCurrentThreadId());
	if (hook == NULL) {
		U_LOG_W("workspace_modal: SetWindowsHookExW(WH_CBT) failed: 0x%08lx", GetLastError());
		DestroyWindow(owner);
		return;
	}

	EnterCriticalSection(&s_lock);
	s_app_hidden_hwnd = (HWND)app_hwnd;
	s_dialog_owner_hwnd = owner;
	s_cbt_hook = hook;
	s_workspace_xc = (sess->xcn != NULL) ? &sess->xcn->base : NULL;
	s_modal_open_depth = 0;
	s_tracked_dialog_count = 0;
	LeaveCriticalSection(&s_lock);

	// Stash the opaque handles on the session for fini and so the rest of
	// the OXR layer can see we're active. void* on the struct because
	// oxr_objects.h is included by non-Win32 TUs that don't have HWND.
	sess->workspace_modal_dialog_owner = (void *)owner;
	sess->workspace_modal_cbt_hook = (void *)hook;

	U_LOG_W("workspace_modal: installed (app_hwnd=%p, owner=%p, hook=%p)",
	        (void *)app_hwnd, (void *)owner, (void *)hook);
}

void
oxr_workspace_modal_win32_fini(struct oxr_session *sess)
{
	if (sess == NULL) {
		return;
	}
	HHOOK hook = (HHOOK)sess->workspace_modal_cbt_hook;
	HWND owner = (HWND)sess->workspace_modal_dialog_owner;
	sess->workspace_modal_cbt_hook = NULL;
	sess->workspace_modal_dialog_owner = NULL;

	if (!s_lock_initialized) {
		// init never ran — nothing to clean up. Stashed handles, if
		// any, came from somewhere unexpected; ignore.
		return;
	}

	// Process globals cleared first so any in-flight hook callback no-ops.
	EnterCriticalSection(&s_lock);
	bool was_active = (s_cbt_hook == hook && hook != NULL);
	if (was_active) {
		s_app_hidden_hwnd = NULL;
		s_dialog_owner_hwnd = NULL;
		s_cbt_hook = NULL;
		s_workspace_xc = NULL;
		s_modal_open_depth = 0;
		s_tracked_dialog_count = 0;
	}
	LeaveCriticalSection(&s_lock);

	if (was_active) {
		if (hook != NULL) {
			(void)UnhookWindowsHookEx(hook);
		}
		if (owner != NULL) {
			(void)DestroyWindow(owner);
		}
		U_LOG_W("workspace_modal: uninstalled");
	}
}

#else // !XRT_OS_WINDOWS

// Empty TU on non-Windows so the build system can include this
// unconditionally (CMake target list is per-platform but having a real .c
// file simplifies generators that don't gate by source presence).
typedef int oxr_workspace_modal_win32_unit_translation_dummy_t;

#endif // XRT_OS_WINDOWS
