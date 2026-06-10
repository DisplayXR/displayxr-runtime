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
#include <stdint.h>
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

// spec_version 24: when the runtime forwards a keystroke to a composited IPC app
// via PostMessage, it stamps the current modifier mask into spare lParam bits —
// PostMessage cannot carry keyboard state across processes, so the app thread's
// GetKeyState / TranslateMessage would otherwise see no modifiers (Ctrl+L → 'l',
// Shift+Tab → bare Tab). The WH_GETMESSAGE hook below decodes the mask and
// SetKeyboardState's the app thread to match before the app's own message loop
// translates / dispatches the key. Bit 28 marks a forwarded message; bits 25-27
// hold SHIFT/CTRL/ALT. KEEP IN SYNC with the encoder in comp_d3d11_window.cpp.
#define DXR_FWD_KEY_MARKER_BIT  (1u << 28)
#define DXR_FWD_KEY_MODS_SHIFT  25
#define DXR_FWD_KEY_BITS_MASK   (0xFu << DXR_FWD_KEY_MODS_SHIFT) // bits 25-28

static HWND s_app_hidden_hwnd = NULL;
static HWND s_dialog_owner_hwnd = NULL;
static HHOOK s_cbt_hook = NULL;
static HHOOK s_getmsg_hook = NULL;
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

			// #232 item 2: also flag the dialog as WS_EX_TOPMOST so it
			// sits visually above the workspace compositor's swap-chain
			// window regardless of activation z-order swaps. Without
			// this, the user clicking into the compositor raises the
			// compositor in non-topmost z and obscures the dialog while
			// the dialog still holds keyboard focus — keystrokes route
			// to an invisible window, which reads as broken.
			//
			// Mutating dwExStyle here works the same way as the parent
			// rewrite above: CreateWindowEx reads the modified lpcs
			// value when it actually creates the window. No SetWindowPos
			// is needed (and wouldn't work — the HWND doesn't exist yet
			// at HCBT_CREATEWND). Nested modals each get the same flag
			// individually; on close they're destroyed which discards
			// the bit, so there's no cleanup pair.
			cbt->lpcs->dwExStyle |= WS_EX_TOPMOST;

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
	} else if (code == HCBT_ACTIVATE) {
		// #232 follow-up: re-assert HWND_TOPMOST on every activation of a
		// tracked dialog. The lpcs.dwExStyle |= WS_EX_TOPMOST mutation at
		// HCBT_CREATEWND sets the initial state correctly for the FIRST
		// dialog (which is why it appears in front the first time), but
		// the dialog's own activation churn (modal-disable of the owner
		// chain, focus-restore from the previous dialog's destroy, etc.)
		// can drop the topmost bit on subsequent dialogs in the same
		// process. The visible symptom is "second L-press dialog opens
		// BEHIND the workspace compositor" — keyboard focus is correct
		// (Alt+Tab brings it forward) but z-order is wrong. Forcing
		// HWND_TOPMOST here, with NOMOVE | NOSIZE | NOACTIVATE so we
		// don't perturb the dialog's own positioning or focus state,
		// keeps every dialog in this session above non-topmost windows.
		HWND hwnd = (HWND)wParam;
		EnterCriticalSection(&s_lock);
		bool is_tracked = false;
		for (int i = 0; i < s_tracked_dialog_count; i++) {
			if (s_tracked_dialogs[i] == hwnd) {
				is_tracked = true;
				break;
			}
		}
		LeaveCriticalSection(&s_lock);
		if (is_tracked) {
			(void)SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
			                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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

// Force the app thread's keyboard state to match the forwarded modifier mask
// (bit0=SHIFT, bit1=CTRL, bit2=ALT) so the app's own TranslateMessage and any
// GetKeyState in its WindowProc see the real chord. Runs on the app UI thread.
static void
apply_forwarded_modifiers(uint32_t mods)
{
	BYTE state[256];
	if (!GetKeyboardState(state)) {
		return;
	}
	BYTE shift = (mods & 0x1u) ? (BYTE)0x80 : (BYTE)0x00;
	BYTE ctrl  = (mods & 0x2u) ? (BYTE)0x80 : (BYTE)0x00;
	BYTE alt   = (mods & 0x4u) ? (BYTE)0x80 : (BYTE)0x00;
	// Set both the generic and the left-hand specific VKs so GetKeyState
	// (generic) and TranslateMessage both observe the right state.
	state[VK_SHIFT]    = shift;
	state[VK_LSHIFT]   = shift;
	state[VK_CONTROL]  = ctrl;
	state[VK_LCONTROL] = ctrl;
	state[VK_MENU]     = alt;
	state[VK_LMENU]    = alt;
	(void)SetKeyboardState(state);
}

// WH_GETMESSAGE hook on the app UI thread. Fires from inside the app's
// GetMessage/PeekMessage, before it TranslateMessage's / DispatchMessage's the
// message — exactly where the keyboard state must be right. For forwarded key
// messages (marker bit set) we apply the carried modifier mask and strip our
// private lParam bits so the app sees a clean message.
static LRESULT CALLBACK
getmsg_hook_proc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code == HC_ACTION && wParam == PM_REMOVE) {
		MSG *msg = (MSG *)lParam;
		if (msg != NULL) {
			switch (msg->message) {
			case WM_KEYDOWN:
			case WM_KEYUP:
			case WM_SYSKEYDOWN:
			case WM_SYSKEYUP:
			case WM_CHAR:
			case WM_SYSCHAR:
				if (msg->lParam & (LPARAM)DXR_FWD_KEY_MARKER_BIT) {
					uint32_t mods = (uint32_t)((msg->lParam >> DXR_FWD_KEY_MODS_SHIFT) & 0x7u);
					apply_forwarded_modifiers(mods);
					msg->lParam &= ~(LPARAM)DXR_FWD_KEY_BITS_MASK;
				}
				break;
			default:
				break;
			}
		}
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

	// spec_version 24: per-thread WH_GETMESSAGE hook that restores forwarded
	// modifier state before the app translates/dispatches the key. Same thread
	// scope as the CBT hook. A failure here is non-fatal — chord forwarding
	// degrades but modal re-parenting still works — so we keep going.
	HHOOK getmsg = SetWindowsHookExW(WH_GETMESSAGE, getmsg_hook_proc, NULL, GetCurrentThreadId());
	if (getmsg == NULL) {
		U_LOG_W("workspace_modal: SetWindowsHookExW(WH_GETMESSAGE) failed: 0x%08lx — "
		        "forwarded key chords may lose modifiers",
		        GetLastError());
	}

	EnterCriticalSection(&s_lock);
	s_app_hidden_hwnd = (HWND)app_hwnd;
	s_dialog_owner_hwnd = owner;
	s_cbt_hook = hook;
	s_getmsg_hook = getmsg;
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
	HHOOK getmsg = NULL;
	if (was_active) {
		s_app_hidden_hwnd = NULL;
		s_dialog_owner_hwnd = NULL;
		s_cbt_hook = NULL;
		getmsg = s_getmsg_hook; // unhook outside the lock (paired with the CBT hook)
		s_getmsg_hook = NULL;
		s_workspace_xc = NULL;
		s_modal_open_depth = 0;
		s_tracked_dialog_count = 0;
	}
	LeaveCriticalSection(&s_lock);

	if (was_active) {
		if (hook != NULL) {
			(void)UnhookWindowsHookEx(hook);
		}
		if (getmsg != NULL) {
			(void)UnhookWindowsHookEx(getmsg);
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
